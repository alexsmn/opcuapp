#pragma once

#include "opcua/scada/basic_types.h"
#include "opcua/scada/date_time.h"
#include "opcua/scada/expanded_node_id.h"
#include "opcua/scada/extension_object.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/qualified_name.h"
#include "opcua/scada/string.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace opcua {

// Built-in OPC UA Variant: a union that can hold a scalar or array of any other
// built-in type, used wherever a value of dynamic type is carried. OPC UA Part 6
// §5.1.9 Variant, https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.9
class Variant {
 public:
  // opcuapp representation of the built-in type held by a Variant; the closest
  // spec concept is the Variant BuiltInType / encoding mask. OPC UA Part 6
  // §5.1.9 Variant, https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.9
  enum Type {
    EMPTY,
    BOOL,
    INT8,
    UINT8,
    INT16,
    UINT16,
    INT32,
    UINT32,
    INT64,
    UINT64,
    DOUBLE,
    BYTE_STRING,
    STRING,
    QUALIFIED_NAME,
    LOCALIZED_TEXT,
    NODE_ID,
    EXPANDED_NODE_ID,
    EXTENSION_OBJECT,
    DATE_TIME,
    COUNT
  };

  constexpr Variant() noexcept = default;
  constexpr Variant(bool value) noexcept : data_{value} {}
  constexpr Variant(Int8 value) noexcept : data_{value} {}
  constexpr Variant(UInt8 value) noexcept : data_{value} {}
  constexpr Variant(Int16 value) noexcept : data_{value} {}
  constexpr Variant(UInt16 value) noexcept : data_{value} {}
  constexpr Variant(Int32 value) noexcept : data_{value} {}
  constexpr Variant(UInt32 value) noexcept : data_{value} {}
  constexpr Variant(Int64 value) noexcept : data_{value} {}
  constexpr Variant(UInt64 value) noexcept : data_{value} {}
  constexpr Variant(Double value) noexcept : data_{value} {}
  Variant(ByteString str) noexcept : data_{std::move(str)} {}
  Variant(String str) noexcept : data_{std::move(str)} {}
  Variant(QualifiedName value) noexcept : data_{std::move(value)} {}
  Variant(LocalizedText str) noexcept : data_{std::move(str)} {}
  constexpr Variant(DateTime value) noexcept : data_{value} {}
  Variant(const char* str) : data_{str ? String{str} : String{}} {}
  Variant(const char16_t* str)
      : data_{str ? LocalizedText{str} : LocalizedText{}} {}
  Variant(NodeId node_id) noexcept : data_{std::move(node_id)} {}
  Variant(ExpandedNodeId node_id) noexcept : data_{std::move(node_id)} {}
  Variant(ExtensionObject source) noexcept : data_{std::move(source)} {}
  template <class T>
  Variant(std::vector<T> value) noexcept : data_{std::move(value)} {}

  Variant(const Variant& source) = default;
  Variant(Variant&& source) noexcept = default;

  ~Variant() { clear(); }

  void clear();

  constexpr bool is_null() const noexcept { return type() == EMPTY; }

  constexpr Type type() const noexcept;
  constexpr bool is_scalar() const noexcept;
  constexpr bool is_array() const noexcept { return !is_scalar(); }

  NodeId data_type_id() const;

  bool as_bool() const { return std::get<bool>(data_); }
  Int32 as_int32() const { return std::get<Int32>(data_); }
  UInt32 as_uint32() const { return std::get<UInt32>(data_); }
  Int64 as_int64() const { return std::get<Int64>(data_); }
  UInt64 as_uint64() const { return std::get<UInt64>(data_); }
  Double as_double() const { return std::get<Double>(data_); }
  const String& as_string() const { return std::get<String>(data_); }
  const LocalizedText& as_localized_text() const {
    return std::get<LocalizedText>(data_);
  }
  const NodeId& as_node_id() const { return std::get<NodeId>(data_); }

  template <class T>
  constexpr const T& get() const {
    return std::get<T>(data_);
  }
  template <class T>
  constexpr T& get() {
    return std::get<T>(data_);
  }

  bool get(bool& bool_value) const;
  bool get(Int8& value) const { return get_int<Int8>(value); }
  bool get(UInt8& value) const { return get_int<UInt8>(value); }
  bool get(Int16& value) const { return get_int<Int16>(value); }
  bool get(UInt16& value) const { return get_int<UInt16>(value); }
  bool get(Int32& value) const { return get_int<Int32>(value); }
  bool get(UInt32& value) const { return get_int<UInt32>(value); }
  bool get(Int64& value) const;
  bool get(UInt64& value) const { return get_int<UInt64>(value); }
  bool get(Double& value) const;
  bool get(String& value) const;
  bool get(QualifiedName& value) const;
  bool get(LocalizedText& value) const;
  bool get(NodeId& value) const;

  template <class T>
  bool get(T& value) const;

  template <class T>
  T get_or(T or_value) const;

  template <class T>
  constexpr T* get_if() noexcept;

  template <class T>
  constexpr const T* get_if() const noexcept;

  Variant& operator=(const Variant& source) = default;
  Variant& operator=(Variant&& source) = default;

  constexpr bool operator==(const Variant& other) const noexcept;
  constexpr bool operator!=(const Variant& other) const noexcept {
    return !operator==(other);
  }

  bool ChangeType(Variant::Type new_type);
  template <typename T>
  bool ChangeTypeTo();

  void Dump(std::ostream& stream) const;

  static const std::u16string_view kTrueString;
  static const std::u16string_view kFalseString;

 private:
  template <class T>
  constexpr bool get_int(T& value) const;

  template <class String>
  bool ToStringHelper(String& string_value) const;

  std::variant<std::monostate,
               bool,
               Int8,
               UInt8,
               Int16,
               UInt16,
               Int32,
               UInt32,
               Int64,
               UInt64,
               double,
               ByteString,
               String,
               QualifiedName,
               LocalizedText,
               NodeId,
               ExpandedNodeId,
               ExtensionObject,
               DateTime,
               std::vector<std::monostate>,
               std::vector<bool>,
               std::vector<Int8>,
               std::vector<UInt8>,
               std::vector<Int16>,
               std::vector<UInt16>,
               std::vector<Int32>,
               std::vector<UInt32>,
               std::vector<Int64>,
               std::vector<UInt64>,
               std::vector<Double>,
               std::vector<ByteString>,
               std::vector<String>,
               std::vector<QualifiedName>,
               std::vector<LocalizedText>,
               std::vector<NodeId>,
               std::vector<ExpandedNodeId>,
               std::vector<ExtensionObject> >
      data_;
};

inline constexpr Variant::Type Variant::type() const noexcept {
  if (data_.index() < static_cast<size_t>(Type::COUNT))
    return static_cast<Type>(data_.index());
  else
    return static_cast<Type>(data_.index() - static_cast<size_t>(Type::COUNT));
}

inline constexpr bool Variant::is_scalar() const noexcept {
  return data_.index() < static_cast<size_t>(Type::COUNT);
}

inline constexpr bool Variant::operator==(const Variant& other) const noexcept {
  if (type() == Type::DOUBLE || other.type() == DOUBLE) {
    double a = 0.0, b = 0.0;
    return get(a) && other.get(b) &&
           std::abs(a - b) < std::numeric_limits<double>::epsilon();
  }

  return data_ == other.data_;
}

template <class T>
inline bool Variant::ChangeTypeTo() {
  T value;
  if (!get(value))
    return false;
  data_ = std::move(value);
  return true;
}

template <class T>
inline bool Variant::get(T& value) const {
  auto* ptr = get_if<T>();
  if (!ptr)
    return false;
  value = *ptr;
  return true;
}

template <class T>
inline constexpr bool Variant::get_int(T& value) const {
  Int64 int64_value;
  if (!get(int64_value))
    return false;

  value = static_cast<T>(int64_value);
  return static_cast<Int64>(value) == int64_value;
}

template <class T>
inline constexpr T* Variant::get_if() noexcept {
  return std::get_if<T>(&data_);
}

template <class T>
inline constexpr const T* Variant::get_if() const noexcept {
  return std::get_if<T>(&data_);
}

template <class T>
inline T Variant::get_or(T or_value) const {
  auto result = std::move(or_value);
  get(result);
  return result;
}

NodeId ToNodeId(Variant::Type type);


std::string ToString(opcua::Variant::Type type);

std::string ToString(const opcua::Variant& value);
std::u16string ToString16(const opcua::Variant& value);


inline std::ostream& operator<<(std::ostream& stream, const Variant& v) {
  v.Dump(stream);
  return stream;
}

inline std::ostream& operator<<(std::ostream& stream, Variant::Type type) {
  return stream << ToString(type);
}

}  // namespace opcua (vendored)
