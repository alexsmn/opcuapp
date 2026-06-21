#pragma once

#include "transport/error.h"

#include <boost/asio/ssl/context.hpp>

#include <string>

namespace opcua::ws {

struct TlsContextConfig {
  std::string certificate_chain_pem;
  std::string private_key_pem;
  std::string private_key_passphrase;
};

[[nodiscard]] transport::error_code ConfigureServerTlsContext(
    boost::asio::ssl::context& context,
    const TlsContextConfig& config);

}  // namespace opcua::ws
