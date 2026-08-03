#pragma once

#include <span>
#include <string>
#include <string_view>

namespace opcua {

// Renders the set bits of `bit_mask` as a JSON-style array of their labels,
// e.g. bits {0, 2} with labels {"A", "B", "C"} produce ["A","C"]. `labels[i]`
// names bit i. Bits without a label, and bits beyond the width of `unsigned`,
// are ignored.
std::string BitMaskToString(unsigned bit_mask,
                            std::span<const std::string_view> labels);

}  // namespace opcua
