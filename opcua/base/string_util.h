#pragma once

#include <string>
#include <string_view>
#include <vector>

// SplitString: split by delimiter substring (not character set).
// boost::split is not useful for delimiter string.
namespace opcua {
std::vector<std::string_view> SplitString(std::string_view str,
                                          std::string_view delimiter);

inline std::vector<std::string_view> SplitString(std::string_view str,
                                                 char delimiter) {
  return SplitString(str, std::string_view{&delimiter, 1});
}
}  // namespace opcua
