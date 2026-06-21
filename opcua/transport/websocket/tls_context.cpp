#include "opcua/websocket/tls_context.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ssl/context_base.hpp>

namespace opcua::ws {

transport::error_code ConfigureServerTlsContext(
    boost::asio::ssl::context& context,
    const TlsContextConfig& config) {
  boost::system::error_code ec;

  context.set_options(boost::asio::ssl::context::default_workarounds |
                          boost::asio::ssl::context::no_sslv2 |
                          boost::asio::ssl::context::no_sslv3 |
                          boost::asio::ssl::context::single_dh_use,
                      ec);
  if (ec)
    return ec;

  if (!config.private_key_passphrase.empty()) {
    context.set_password_callback(
        [password = config.private_key_passphrase](
            std::size_t,
            boost::asio::ssl::context_base::password_purpose) {
          return password;
        });
  }

  context.use_certificate_chain(
      boost::asio::buffer(config.certificate_chain_pem), ec);
  if (ec)
    return ec;

  context.use_private_key(boost::asio::buffer(config.private_key_pem),
                          boost::asio::ssl::context::file_format::pem, ec);
  if (ec)
    return ec;

  return transport::OK;
}

}  // namespace opcua::ws
