#include <eos/tcp_connection_plugin/tcp_connection.hpp>

#include <fc/log/logger.hpp>

#include <boost/asio.hpp>

namespace eosio {

tcp_connection::tcp_connection(boost::asio::ip::tcp::socket s) :
   _socket(std::move(s)),
   _strand(_socket.get_io_service())
{
   ilog("tcp_connection created");

   fc::ecc::private_key priv_key = fc::ecc::private_key::generate();
   
   fc::ecc::public_key pub = priv_key.get_public_key();
   fc::ecc::public_key_data pkd = pub.serialize();

   boost::system::error_code ec;
   size_t wrote = boost::asio::write(_socket, boost::asio::buffer(pkd.begin(), pkd.size()), ec);
   if(ec || wrote != pkd.size()) {
      handle_failure();
      return;
   }
   boost::asio::async_read(_socket, boost::asio::buffer(_rxbuffer, pkd.size()), [this, priv_key](auto ec, auto r) {
      finish_key_exchange(ec, r, priv_key);
   });
}

tcp_connection::~tcp_connection() {
   ilog("Connection destroyed"); //XXX debug
}

void tcp_connection::finish_key_exchange(boost::system::error_code ec, size_t red, fc::ecc::private_key priv_key) {
   fc::ecc::public_key_data* rpub = (fc::ecc::public_key_data*)_rxbuffer;
   if(ec || red != rpub->size()) {
       handle_failure();
       return;
   }

   fc::sha512 shared_secret;
   try {
      shared_secret = priv_key.get_shared_secret(*rpub);
   }
   catch(fc::assert_exception) {
       elog("Failed to negotiate shared encrpytion secret with ${ip}:${port}. Not an EOSIO peer?",
            ("ip", _socket.remote_endpoint().address().to_string())
            ("port", _socket.remote_endpoint().port())
           );
       handle_failure();
       return;
   }

   auto ss_data = shared_secret.data();
   auto ss_data_size = shared_secret.data_size();

   _sending_aes_enc_ctx.init  (fc::sha256::hash(ss_data, ss_data_size), fc::city_hash_crc_128(ss_data, ss_data_size));
   _receiving_aes_dec_ctx.init(fc::sha256::hash(ss_data, ss_data_size), fc::city_hash_crc_128(ss_data, ss_data_size));

   read();
}

//Threading strategy: The RX and TX callbacks will not be wrapped -- they may indeed be called simulatnously
// from different threads. However, need to make sure that any calls _back_ to the socket object are
// serialized. This includes new async_reads, async_writes, or even close()s.

void tcp_connection::handle_failure() {
   _strand.dispatch([this]() {
      _socket.cancel();
      _socket.close();
      if(!_disconnected_fired)
         _on_disconnected_sig();
      _disconnected_fired = true;
   });
}

void tcp_connection::read() {
   _strand.dispatch([this]() {
      auto read_buffer_offset = boost::asio::mutable_buffers_1(boost::asio::buffer(_rxbuffer)+_rxbuffer_leftover);
      _socket.async_read_some(read_buffer_offset, [this](auto ec, auto r) {
         read_ready(ec, r);
      });
   });
}

void tcp_connection::read_ready(boost::system::error_code ec, size_t red) {
   if(ec) {
      handle_failure();
      return;
   }
   //the amount read (red), plus the amount left over from last time (rxbuffer_leftover), is what we
   // have to work with entirely
   unsigned int amount_in_rxbuffer = red+_rxbuffer_leftover;
   //but we will only actually decode up to last multiple of AES block size (16 bytes)
   unsigned int amount_decryptable_in_rxbuffer = amount_in_rxbuffer & ~15U;

   size_t to_next_parsebuffer_rollover = sizeof(_parsebuffer)-_parsebuffer_head%sizeof(_parsebuffer);
   if(amount_decryptable_in_rxbuffer > to_next_parsebuffer_rollover) {
      size_t first_size = to_next_parsebuffer_rollover;
      _parsebuffer_head += _receiving_aes_dec_ctx.decode(_rxbuffer, first_size, _parsebuffer + _parsebuffer_head%sizeof(_parsebuffer));  
      _parsebuffer_head += _receiving_aes_dec_ctx.decode(_rxbuffer + first_size, amount_decryptable_in_rxbuffer - first_size, _parsebuffer); 
   }
   else 
      _parsebuffer_head += _receiving_aes_dec_ctx.decode(_rxbuffer, amount_decryptable_in_rxbuffer, _parsebuffer + _parsebuffer_head);

   //whatever 0-15 bytes are leftover in rxbuffer; save them to next time through
   _rxbuffer_leftover = amount_in_rxbuffer % 16;
   memmove(_rxbuffer, _rxbuffer+amount_decryptable_in_rxbuffer, _rxbuffer_leftover);

   while(_parsebuffer_tail != _parsebuffer_head) {
      size_t parseable = _parsebuffer_head - _parsebuffer_tail;
      datastream<circular_buffer<const char*>> parse_me(_parsebuffer, sizeof(_parsebuffer), _parsebuffer_tail%sizeof(_parsebuffer), parseable);

      try {
         fc::unsigned_int message_block_size;
         fc::raw::unpack(parse_me, message_block_size);
         if((unsigned int)message_block_size*16 > max_message_size)
            FC_THROW("Recieved message is too large");
         if((unsigned int)message_block_size*16 > parseable)
            break;

         net2_message message;
         fc::raw::unpack(parse_me, message);
         message.visit(*this);
      }
      catch(fc::exception e) {
         wlog("Caught exception while parsing incoming TCP messages; disconnecting: ${error}", ("error", e.what()));
         handle_failure();
         return;
      }

      _parsebuffer_tail += (parse_me.tellp()+15)&~15U;
   }

   read();
}

void tcp_connection::operator()(const handshake2_message& handshake) {

}

void tcp_connection::operator()(const packed_transaction_list& transactions) {
   std::lock_guard<std::mutex> lock(_transaction_mutex);

   for(const meta_transaction_ptr& trx : transactions.incoming_transactions) {
      auto p = _seen_transactions.emplace(trx->id, trx->expiration);
      if(!p.second) //couldn't emplace; we've seen trx in/egress recently already
         continue;

      //controller::add_transaction(move(trx));
   }
}

void tcp_connection::begin_processing_network_send_queue(connection_send_context& context) {
   
}

bool tcp_connection::disconnected() {
   return !_socket.is_open();
}

connection tcp_connection::on_disconnected(const signal<void()>::slot_type& slot) {
   return _on_disconnected_sig.connect(slot);
}

handshake2_message tcp_connection::fill_handshake() {
   handshake2_message hand;

   hand.protocol_message_level = net2_message::count();
   //app().get_plugin<chain_plugin>().get_chain_id(hand.chain_id);;

   return hand;
}

}