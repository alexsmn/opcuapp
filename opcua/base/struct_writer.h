#pragma once

#include "opcua/base/debug_util.h"

#include <format>
#include <ostream>
#include <sstream>
#include <string_view>

// Usage:
// std::ostream& operator<<(std::ostream& stream, const MyType& x) {
//   constexpr std::string_view kVerbStrings[] = {"Bit0", "Bit1", "Bit02"};
//   StructWriter{stream}
//       .AddField("a", x.a)
//       .AddBitMaskField("b", x.b, kEnumStrings)
//       .AddField("c", x.c);
//   return stream;
// }
namespace opcua {
class StructWriter {
 public:
  explicit StructWriter(std::ostream& stream);
  ~StructWriter();

  template <class T>
  StructWriter& AddField(std::string_view name, const T& value);

  StructWriter& AddBitMaskField(std::string_view name,
                                unsigned bit_mask,
                                std::span<const std::string_view> bit_strings);

 private:
  template <class T>
  void AddValue(const T& value);

  std::ostream& stream_;
  std::size_t count_ = 0;
};

inline StructWriter::StructWriter(std::ostream& stream) : stream_{stream} {
  stream_ << "{";
}

inline StructWriter::~StructWriter() {
  stream_ << "}";
}

template <class T>
inline StructWriter& StructWriter::AddField(std::string_view name,
                                            const T& value) {
  if (count_ != 0)
    stream_ << ", ";
  stream_ << name << ": ";
  AddValue(value);
  ++count_;
  return *this;
}

inline StructWriter& StructWriter::AddBitMaskField(
    std::string_view name,
    unsigned bit_mask,
    std::span<const std::string_view> bit_strings) {
  if (count_ != 0)
    stream_ << ", ";
  stream_ << name << ": " << BitMaskToString(bit_mask, bit_strings);
  ++count_;
  return *this;
}

template <class T>
inline void StructWriter::AddValue(const T& value) {
  stream_ << value;
}

template <>
inline void StructWriter::AddValue(const char* const& value) {
  stream_ << "\"" << value << "\"";
}

template <>
inline void StructWriter::AddValue(const std::string& value) {
  stream_ << "\"" << value << "\"";
}

template <>
void StructWriter::AddValue(const std::wstring& value);

template <>
void StructWriter::AddValue(const std::u16string& value);

// std::formatter adapter that renders a type through its
// operator<<(std::ostream&, const T&) — typically one written with
// StructWriter. Opt a type in by deriving its std::formatter specialization
// from this:
//
//   template <>
//   struct std::formatter<opcua::MyType> : opcua::OStreamFormatter {};
//
// MyType then works directly with std::format, std::print, and any other
// std::format-based sink. Only the default "{}" format spec is accepted; a
// non-empty spec is rejected with std::format_error.
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
