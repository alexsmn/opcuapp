#pragma once

#include "opcua/base/debug_util.h"

#include <ostream>
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
