#include "opcua/scada/variant.h"

#include "opcua/base/debug_util.h"
#include "opcua/base/format.h"
#include "opcua/base/format_time.h"
#include "opcua/base/utf_convert.h"
#include "opcua/scada/standard_node_ids.h"

#include <cassert>
#include <limits>

namespace opcua::scada {

namespace {

const char* const kBuiltInDataTypeNames[] = {"EMPTY",
                                             "BOOL",
                                             "INT8",
                                             "UINT8",
                                             "INT16",
                                             "UINT16",
                                             "INT32",
                                             "UINT32",
                                             "UINT64",
                                             "INT64",
                                             "DOUBLE",
                                             "BYTE_STRING",
                                             "STRING",
                                             "QUALIFIED_NAME",
                                             "LOCALIZED_TEXT",
                                             "NODE_ID",
                                             "EXPANDED_NODE_ID",
                                             "EXTENSION_OBJECT",
                                             "DATE_TIME"};

static_assert(std::size(kBuiltInDataTypeNames) ==
              static_cast<size_t>(opcua::scada::Variant::COUNT));

const opcua::scada::NumericId kBuiltInDataTypeNodeIds[] = {/*EMPTY=*/0,
                                                    id::Boolean,
                                                    id::Int8,
                                                    id::UInt8,
                                                    id::Int16,
                                                    id::UInt16,
                                                    id::Int32,
                                                    id::UInt32,
                                                    id::Int64,
                                                    id::UInt64,
                                                    id::Double,
                                                    id::ByteString,
                                                    id::String,
                                                    id::QualifiedName,
                                                    id::LocalizedText,
                                                    id::NodeId,
                                                    id::ExpandedNodeId,
                                                    /*EXTENSION_OBJECT=*/0,
                                                    id::DateTime};

static_assert(std::size(kBuiltInDataTypeNodeIds) ==
              static_cast<size_t>(Variant::Type::COUNT));

}  // namespace

const std::u16string_view Variant::kTrueString = u"Да";
const std::u16string_view Variant::kFalseString = u"Нет";

void Variant::clear() {
  data_ = std::monostate{};
}

bool Variant::get(bool& value) const {
  if (!is_scalar())
    return false;

  if (type() == BOOL) {
    value = as_bool();
    return true;
  }

  Int64 int64_value;
  if (!get(int64_value))
    return false;

  value = int64_value != 0;
  return true;
}

bool Variant::get(Int64& value) const {
  if (!is_scalar())
    return false;

  switch (type()) {
    case BOOL:
      value = as_bool() ? 1 : 0;
      return true;
    case INT8:
      value = get<Int8>();
      return true;
    case UINT8:
      value = get<UInt8>();
      return true;
    case INT16:
      value = get<Int16>();
      return true;
    case UINT16:
      value = get<UInt16>();
      return true;
    case INT32:
      value = get<Int32>();
      return true;
    case UINT32:
      value = get<UInt32>();
      return true;
    case INT64:
      value = get<Int64>();
      return true;
    case UINT64:
      value = static_cast<Int64>(get<UInt64>());
      return true;
    case DOUBLE:
      value = static_cast<Int64>(get<Double>());
      return true;
    default:
      return false;
  }
}

bool Variant::get(Double& value) const {
  if (!is_scalar())
    return false;

  switch (type()) {
    case BOOL:
      value = as_bool() ? 1.0 : 0.0;
      return true;
    case DOUBLE:
      value = as_double();
      return true;
    default:
      break;
  }

  Int64 int64_value;
  if (!get(int64_value))
    return false;

  value = static_cast<Double>(int64_value);
  return true;
}

namespace {

template <class Target, class Source>
struct FormatHelperT;

template <class Source>
struct FormatHelperT<String, Source> {
  static inline String Format(const Source& value) { return ::Format(value); }
};

template <>
struct FormatHelperT<String, QualifiedName> {
  static inline String Format(const QualifiedName& value) {
    return ToString(value);
  }
};

template <>
struct FormatHelperT<String, LocalizedText> {
  static inline String Format(const LocalizedText& value) {
    return ToString(value);
  }
};

template <class Source>
struct FormatHelperT<LocalizedText, Source> {
  static inline LocalizedText Format(const Source& value) {
    return ToLocalizedText(::Format(value));
  }
};

template <>
struct FormatHelperT<LocalizedText, QualifiedName> {
  static inline LocalizedText Format(const QualifiedName& value) {
    return ToString16(value);
  }
};

template <>
struct FormatHelperT<LocalizedText, LocalizedText> {
  static inline LocalizedText Format(const LocalizedText& value) {
    return value;
  }
};

template <>
struct FormatHelperT<String, DateTime> {
  static inline String Format(const DateTime& value) {
    return FormatTime(value);
  }
};

template <>
struct FormatHelperT<LocalizedText, DateTime> {
  static inline LocalizedText Format(const DateTime& value) {
    return ToLocalizedText(FormatTime(value));
  }
};

template <>
struct FormatHelperT<String, bool> {
  static inline String Format(const bool& value) { return value ? "1" : "0"; }
};

template <>
struct FormatHelperT<LocalizedText, bool> {
  static inline LocalizedText Format(const bool& value) {
    return value ? LocalizedText{Variant::kTrueString}
                 : LocalizedText{Variant::kFalseString};
  }
};

template <class Target, class Source>
inline Target FormatHelper(const Source& value) {
  return FormatHelperT<Target, Source>::Format(value);
}

}  // namespace

template <class String>
bool Variant::ToStringHelper(String& string_value) const {
  if (!is_scalar())
    return false;

  switch (type()) {
    case EMPTY:
      string_value.clear();
      return true;
    case BOOL:
      string_value = FormatHelper<String>(as_bool());
      return true;
    case DOUBLE:
      string_value = FormatHelper<String>(as_double());
      return true;
    case STRING:
      string_value = FormatHelper<String>(as_string());
      return true;
    case QUALIFIED_NAME:
      string_value = FormatHelper<String>(get<QualifiedName>());
      return true;
    case LOCALIZED_TEXT:
      string_value = FormatHelper<String>(as_localized_text());
      return true;
    case NODE_ID:
      string_value = FormatHelper<String>(as_node_id().ToString());
      return true;
    case DATE_TIME:
      string_value = FormatHelper<String>(get<DateTime>());
      return true;
    default: {
      Int64 int64_value;
      if (get(int64_value)) {
        string_value = FormatHelper<String>(int64_value);
        return true;
      }
      return false;
    }
  }
}

bool Variant::get(String& string_value) const {
  return ToStringHelper(string_value);
}

bool Variant::get(QualifiedName& value) const {
  if (!is_scalar() || type() != QUALIFIED_NAME)
    return false;
  value = get<QualifiedName>();
  return true;
}

bool Variant::get(LocalizedText& value) const {
  return ToStringHelper(value);
}

bool Variant::get(NodeId& node_id) const {
  if (!is_scalar() || type() != NODE_ID)
    return false;
  node_id = as_node_id();
  return true;
}

bool Variant::ChangeType(Variant::Type new_type) {
  assert(new_type != Variant::Type::EMPTY);

  if (!is_scalar())
    return false;

  if (type() == new_type)
    return true;

  switch (new_type) {
    case BOOL:
      return ChangeTypeTo<bool>();
    case INT8:
      return ChangeTypeTo<Int8>();
    case UINT8:
      return ChangeTypeTo<UInt8>();
    case INT16:
      return ChangeTypeTo<Int16>();
    case UINT16:
      return ChangeTypeTo<UInt16>();
    case INT32:
      return ChangeTypeTo<Int32>();
    case UINT32:
      return ChangeTypeTo<UInt32>();
    case INT64:
      return ChangeTypeTo<Int64>();
    case UINT64:
      return ChangeTypeTo<UInt64>();
    case DOUBLE:
      return ChangeTypeTo<double>();
    case STRING:
      return ChangeTypeTo<String>();
    case QUALIFIED_NAME:
      return ChangeTypeTo<QualifiedName>();
    case LOCALIZED_TEXT:
      return ChangeTypeTo<LocalizedText>();
    case NODE_ID:
      return ChangeTypeTo<NodeId>();
    default:
      assert(false);
      return false;
  }
}

NodeId Variant::data_type_id() const {
  static_assert(static_cast<size_t>(Type::COUNT) == 19);

  if (type() == Type::EXTENSION_OBJECT)
    return get<ExtensionObject>().data_type_id().node_id();

  return ToNodeId(type());
}

template <class T>
inline void DumpHelper(std::ostream& stream, const T& v) {
  stream << v;
}

template <>
inline void DumpHelper(std::ostream& stream, const std::monostate& v) {
  stream << "null";
}

template <>
inline void DumpHelper(std::ostream& stream, const ByteString& v) {
  stream << "\"" << FormatHexBuffer(v.data(), v.size()) << "\"";
}

// std::u16string has no `operator<<(std::ostream&, ...)` — printing it
// directly through the generic DumpHelper template either fails to compile
// or, under MSVC's permissive lookup, picks an overload that silently
// loops (showed up as Phase0Responses test hanging in GTest pretty-print
// of vector<DataValue> with a Variant{LocalizedText}). Convert to UTF-8
// before writing to the stream.
template <>
inline void DumpHelper(std::ostream& stream, const std::u16string& v) {
  stream << "\"" << UtfConvert<char>(v) << "\"";
}

template <class T>
inline void DumpHelper(std::ostream& stream, const std::vector<T>& v) {
  stream << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    DumpHelper(stream, v[i]);
    if (i != v.size() - 1)
      stream << ", ";
  }
  stream << "]";
}

void Variant::Dump(std::ostream& stream) const {
  std::visit([&](const auto& v) { DumpHelper(stream, v); }, data_);
}

opcua::scada::Variant::Type ParseBuiltInType(std::string_view str) {
  auto i = std::ranges::find(kBuiltInDataTypeNames, str);
  return i != std::end(kBuiltInDataTypeNames)
             ? static_cast<opcua::scada::Variant::Type>(
                   i - std::begin(kBuiltInDataTypeNames))
             : opcua::scada::Variant::COUNT;
}

NodeId ToNodeId(Variant::Type type) {
  assert(type != Variant::Type::COUNT);
  return kBuiltInDataTypeNodeIds[static_cast<size_t>(type)];
}

Variant::Type ToBuiltInDataType(const NodeId& node_id) {
  if (node_id.namespace_index() != 0 ||
      node_id.type() != opcua::scada::NodeIdType::Numeric) {
    return Variant::Type::COUNT;
  }

  auto i = std::ranges::find(kBuiltInDataTypeNodeIds, node_id);
  if (i == std::end(kBuiltInDataTypeNodeIds)) {
    return Variant::Type::COUNT;
  }

  auto index = i - std::begin(kBuiltInDataTypeNodeIds);
  return static_cast<Variant::Type>(index);
}

}  // namespace opcua::scada

std::string ToString(opcua::scada::Variant::Type type) {
  size_t index = static_cast<size_t>(type);
  return index < std::size(opcua::scada::kBuiltInDataTypeNames)
             ? opcua::scada::kBuiltInDataTypeNames[index]
             : "(Unknown)";
}

std::string ToString(const opcua::scada::Variant& value) {
  return value.get_or(std::string{});
}

std::u16string ToString16(const opcua::scada::Variant& value) {
  return value.get_or(std::u16string{});
}
