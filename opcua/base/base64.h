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

// Returns false when `input` is not valid base64. Beast's decoder stops at the
// first character outside the alphabet — including the `=` padding — so it is
// pointed at the input with any trailing padding removed, and a short read on
// what remains means a genuinely malformed body.
inline bool Base64Decode(std::string_view input, std::string* output) {
  namespace b64 = boost::beast::detail::base64;
  const std::string_view unpadded =
      input.substr(0, input.find_last_not_of('=') + 1);
  output->resize(b64::decoded_size(input.size()));
  const auto result =
      b64::decode(output->data(), unpadded.data(), unpadded.size());
  if (result.second != unpadded.size())
    return false;
  output->resize(result.first);
  return true;
}

}  // namespace base
}  // namespace opcua
