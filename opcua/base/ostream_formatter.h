#pragma once

// OStreamFormatter: std::formatter adapter for operator<<-only types.
// Extracted from struct_writer.h so type headers can opt into std::format
// without pulling in the StructWriter machinery.

#include <format>
#include <sstream>

namespace opcua {

// std::formatter adapter that renders a type through its
// operator<<(std::ostream&, const T&) — typically one written with
// StructWriter. Opt a type in by deriving its std::formatter specialization
// from this:
//
//   template <>
//   struct std::formatter<opcua::MyType> : opcua::OStreamFormatter {};
//
// MyType then works directly with std::format, std::print, opcua::AsList /
// AsDict / AsOpt element rendering, and any other std::format-based sink. Only
// the default "{}" format spec is accepted; a non-empty spec is rejected with
// std::format_error.
struct OStreamFormatter {
  constexpr std::format_parse_context::iterator parse(
      std::format_parse_context& ctx) const {
    const std::format_parse_context::iterator it = ctx.begin();
    if (it != ctx.end() && *it != '}')
      throw std::format_error{"OStreamFormatter does not accept a format spec"};
    return it;
  }

  template <class T, class FormatContext>
  auto format(const T& value, FormatContext& ctx) const {
    std::ostringstream stream;
    stream << value;
    return std::format_to(ctx.out(), "{}", std::move(stream).str());
  }
};

}  // namespace opcua
