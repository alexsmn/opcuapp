#include "opcua/types/variant.h"

#include "opcua/base/debug_util.h"
#include "opcua/base/format.h"
#include "opcua/base/format_time.h"
#include "opcua/base/utf_convert.h"
#include "opcua/types/standard_node_ids.h"

#include <cassert>
#include <limits>

namespace opcua {

namespace {

// Indexed by Variant::Type, which is the spec BuiltInType id.
const char* const kBuiltInDataTypeNames[] = {"EMPTY",
                                             "BOOL",
                                             "INT8",
                                             "UINT8",
                                             "INT16",
                                             "UINT16",
                                             "INT32",
                                             "UINT32",
                                             "INT64",
                                             "UINT64",
                                             "FLOAT",
                                             "DOUBLE",
                                             "STRING",
                                             "DATE_TIME",
                                             "GUID",
                                             "BYTE_STRING",
                                             "XML_ELEMENT",
                                             "NODE_ID",
                                             "EXPANDED_NODE_ID",
                                             "STATUS_CODE",
                                             "QUALIFIED_NAME",
                                             "LOCALIZED_TEXT",
                                             "EXTENSION_OBJECT",
                                             "DATA_VALUE",
                                             "VARIANT",
                                             "DIAGNOSTIC_INFO"};

static_assert(std::size(kBuiltInDataTypeNames) ==
              static_cast<size_t>(opcua::Variant::COUNT));

// The DataType NodeId of every built-in type is its BuiltInType id in
// namespace 0, so no lookup table is needed — but the correspondence is worth
// pinning down, since the whole codec layer now relies on it. OPC UA Part 6
// §A.1 NodeIds, https://reference.opcfoundation.org/Core/Part6/v105/docs/A.1
static_assert(static_cast<opcua::NumericId>(Variant::BOOL) == id::Boolean);
static_assert(static_cast<opcua::NumericId>(Variant::FLOAT) == id::Float);
static_assert(static_cast<opcua::NumericId>(Variant::DOUBLE) == id::Double);
static_assert(static_cast<opcua::NumericId>(Variant::STRING) == id::String);
static_assert(static_cast<opcua::NumericId>(Variant::DATE_TIME) ==
              id::DateTime);
static_assert(static_cast<opcua::NumericId>(Variant::GUID) == id::Guid);
static_assert(static_cast<opcua::NumericId>(Variant::BYTE_STRING) ==
              id::ByteString);
static_assert(static_cast<opcua::NumericId>(Variant::XML_ELEMENT) ==
              id::XmlElement);
static_assert(static_cast<opcua::NumericId>(Variant::NODE_ID) == id::NodeId);
static_assert(static_cast<opcua::NumericId>(Variant::STATUS_CODE) ==
              id::StatusCode);
static_assert(static_cast<opcua::NumericId>(Variant::LOCALIZED_TEXT) ==
              id::LocalizedText);
static_assert(static_cast<opcua::NumericId>(Variant::DATA_VALUE) ==
              id::DataValue);
static_assert(static_cast<opcua::NumericId>(Variant::DIAGNOSTIC_INFO) ==
              id::DiagnosticInfo);

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
  static inline String Format(const Source& value) {
    return opcua::Format(value);
  }
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
    return ToLocalizedText(opcua::Format(value));
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

// LocalizedText's std::u16string payload has no
// `operator<<(std::ostream&, ...)` — printing it directly through the generic
// DumpHelper template either fails to compile or, under MSVC's permissive
// lookup, picks an overload that silently loops (showed up as Phase0Responses
// test hanging in GTest pretty-print of vector<DataValue> with a
// Variant{LocalizedText}). Convert to UTF-8 before writing to the stream.
template <>
inline void DumpHelper(std::ostream& stream, const LocalizedText& v) {
  stream << "\"" << UtfConvert<char>(v.text) << "\"";
  if (!v.locale.empty())
    stream << "@" << v.locale;
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

NodeId ToNodeId(Variant::Type type) {
  assert(type != Variant::Type::COUNT);
  // The DataType NodeId of a built-in type is its BuiltInType id in ns 0; EMPTY
  // (0) has no DataType Node and yields the null NodeId, which is what a
  // NumericId of 0 already means.
  return NodeId{static_cast<NumericId>(type)};
}

std::string ToString(opcua::Variant::Type type) {
  size_t index = static_cast<size_t>(type);
  return index < std::size(opcua::kBuiltInDataTypeNames)
             ? opcua::kBuiltInDataTypeNames[index]
             : "(Unknown)";
}

std::string ToString(const opcua::Variant& value) {
  return value.get_or(std::string{});
}

std::u16string ToString16(const opcua::Variant& value) {
  // Formats through the LocalizedText conversion path (std::u16string is no
  // longer a Variant alternative itself); the locale is irrelevant here.
  return std::move(value.get_or(LocalizedText{}).text);
}
}  // namespace opcua
