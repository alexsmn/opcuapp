#pragma once

#include <boost/beast/core/detail/base64.hpp>
#include <string>
#include <string_view>

namespace opcua {
namespace base {

inline void Base64Encode(std::string_view input, std::string* output) {
  namespace b64 = boost::beast::detail::base64;
  output->resize(b64::encoded_size(input.size()));
  output->resize(b64::encode(output->data(), input.data(), input.size()));
}

}  // namespace base
}  // namespace opcua (vendored)
