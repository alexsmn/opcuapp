#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/string.h"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

namespace opcua {

// Built-in OPC UA Guid: a 16-byte globally unique identifier. The four fields
// mirror the structure the spec defines, which is also what the Binary encoding
// writes on the wire (Data1/Data2/Data3 little-endian, then Data4 verbatim), so
// no byte-order convention has to be assumed by callers. OPC UA Part 3 §8.14
// Guid, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.14
struct Guid {
  std::uint32_t data1 = 0;
  std::uint16_t data2 = 0;
  std::uint16_t data3 = 0;
  std::array<std::uint8_t, 8> data4 = {};

  constexpr bool is_null() const noexcept { return *this == Guid{}; }

  constexpr bool operator==(const Guid& other) const noexcept = default;
  constexpr std::strong_ordering operator<=>(const Guid& other) const noexcept =
      default;

  // The canonical 36-character text form, upper case and hyphen separated:
  // "C496578A-0DFE-4B8F-870A-745238C6AEAE". OPC UA Part 6 §5.1.3 Guid,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.3
  String ToString() const;

  // Parses the canonical text form. Accepts either case; returns nullopt for
  // anything else, including the brace-wrapped Microsoft form.
  static std::optional<Guid> FromString(std::string_view text);
};

inline std::ostream& operator<<(std::ostream& stream, const Guid& guid) {
  return stream << '"' << guid.ToString() << '"';
}

inline String ToString(const Guid& guid) {
  return guid.ToString();
}

}  // namespace opcua
