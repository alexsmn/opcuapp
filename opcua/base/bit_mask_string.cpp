#include "opcua/base/bit_mask_string.h"

#include <algorithm>
#include <limits>

namespace opcua {

std::string BitMaskToString(unsigned bit_mask,
                            std::span<const std::string_view> labels) {
  // Cap the loop at the width of `unsigned`: bits above that can never be set
  // in a 32-bit mask, and `1u << i` for i >= 32 would be undefined behavior.
  constexpr std::size_t kBits = std::numeric_limits<unsigned>::digits;
  const std::size_t count = std::min(labels.size(), kBits);

  std::string result = "[";
  bool first = true;
  for (std::size_t i = 0; i < count; ++i) {
    if ((bit_mask >> i) & 1u) {
      if (!first)
        result += ",";
      result += '"';
      result += labels[i];
      result += '"';
      first = false;
    }
  }
  result += "]";
  return result;
}

}  // namespace opcua
