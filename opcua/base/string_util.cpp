#include "opcua/base/string_util.h"

// SplitString: split by delimiter substring.
namespace opcua {
std::vector<std::string_view> SplitString(std::string_view str,
                                          std::string_view delimiter) {
  if (str.empty())
    return {};

  std::vector<std::string_view> parts;

  for (;;) {
    auto p = str.find(delimiter);
    if (p == delimiter.npos) {
      parts.push_back(str);
      break;
    }

    parts.push_back(str.substr(0, p));
    str = str.substr(p + delimiter.size());
  }

  return parts;
}
}  // namespace opcua (vendored)
