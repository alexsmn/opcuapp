#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/data_value.h"
#include "opcua/types/diagnostic_info.h"
#include "opcua/types/expanded_node_id.h"
#include "opcua/types/extension_object.h"
#include "opcua/types/guid.h"
#include "opcua/types/localized_text.h"
#include "opcua/types/node_id.h"
#include "opcua/types/qualified_name.h"
#include "opcua/types/status.h"
#include "opcua/types/string.h"
#include "opcua/types/variant.h"
#include "opcua/types/xml_element.h"

#include <boost/json/value.hpp>

#include <stdexcept>
#include <string_view>

// The OPC UA JSON encoding of the built-in types (OPC UA Part 6 §5.4,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4), in the compact
// form the OPC Foundation's published service schema describes
// (schema/opc.ua.services.jsonschema.json). The generated per-struct codec in
// ua_json_codec.h bottoms out here, exactly as the binary codec bottoms out in
// codec_utils.cpp.
//
// Compact form means: a field is omitted when it holds its default, arrays are
// JSON arrays, and several built-ins take a text form rather than an object —
// notably QualifiedName ("2:Name") and NodeId ("ns=2;i=5"). A Variant is
// `{UaType, Value, Dimensions}`, and a DataValue *inlines* those three
// alongside its own fields rather than nesting a Variant object.
namespace opcua::ua::json {

// Thrown for malformed input. The generated decoder does not check every field
// individually; it lets this propagate to the transport, which turns it into a
// Bad_DecodingError.
class Error : public std::runtime_error {
 public:
  explicit Error(std::string_view message)
      : std::runtime_error{std::string{message}} {}
};

[[noreturn]] void ThrowError(std::string_view message);

// Scalar built-ins. `Encode` returns the JSON representation; `Decode` throws
// json::Error if the input does not match it.
boost::json::value Encode(Boolean value);
boost::json::value Encode(Int8 value);
boost::json::value Encode(UInt8 value);
boost::json::value Encode(Int16 value);
boost::json::value Encode(UInt16 value);
boost::json::value Encode(Int32 value);
boost::json::value Encode(UInt32 value);
boost::json::value Encode(Int64 value);
boost::json::value Encode(UInt64 value);
boost::json::value Encode(Float value);
boost::json::value Encode(Double value);
boost::json::value Encode(const String& value);
boost::json::value Encode(DateTime value);
boost::json::value Encode(const Guid& value);
boost::json::value Encode(const ByteString& value);
boost::json::value Encode(const XmlElement& value);
boost::json::value Encode(const NodeId& value);
boost::json::value Encode(const ExpandedNodeId& value);
boost::json::value Encode(Status value);
boost::json::value Encode(const QualifiedName& value);
boost::json::value Encode(const LocalizedText& value);
boost::json::value Encode(const ExtensionObject& value);
boost::json::value Encode(const Variant& value);
boost::json::value Encode(const DataValue& value);
boost::json::value Encode(const DiagnosticInfo& value);

void Decode(const boost::json::value& json, Boolean& value);
void Decode(const boost::json::value& json, Int8& value);
void Decode(const boost::json::value& json, UInt8& value);
void Decode(const boost::json::value& json, Int16& value);
void Decode(const boost::json::value& json, UInt16& value);
void Decode(const boost::json::value& json, Int32& value);
void Decode(const boost::json::value& json, UInt32& value);
void Decode(const boost::json::value& json, Int64& value);
void Decode(const boost::json::value& json, UInt64& value);
void Decode(const boost::json::value& json, Float& value);
void Decode(const boost::json::value& json, Double& value);
void Decode(const boost::json::value& json, String& value);
void Decode(const boost::json::value& json, DateTime& value);
void Decode(const boost::json::value& json, Guid& value);
void Decode(const boost::json::value& json, ByteString& value);
void Decode(const boost::json::value& json, XmlElement& value);
void Decode(const boost::json::value& json, NodeId& value);
void Decode(const boost::json::value& json, ExpandedNodeId& value);
void Decode(const boost::json::value& json, Status& value);
void Decode(const boost::json::value& json, QualifiedName& value);
void Decode(const boost::json::value& json, LocalizedText& value);
void Decode(const boost::json::value& json, ExtensionObject& value);
void Decode(const boost::json::value& json, Variant& value);
void Decode(const boost::json::value& json, DataValue& value);
void Decode(const boost::json::value& json, DiagnosticInfo& value);

// Whether a value is at its default and so is omitted from the compact
// encoding. The generated struct encoder calls this per field.
bool IsDefault(Boolean value);
bool IsDefault(Int8 value);
bool IsDefault(UInt8 value);
bool IsDefault(Int16 value);
bool IsDefault(UInt16 value);
bool IsDefault(Int32 value);
bool IsDefault(UInt32 value);
bool IsDefault(Int64 value);
bool IsDefault(UInt64 value);
bool IsDefault(Float value);
bool IsDefault(Double value);
bool IsDefault(const String& value);
bool IsDefault(DateTime value);
bool IsDefault(const Guid& value);
bool IsDefault(const ByteString& value);
bool IsDefault(const XmlElement& value);
bool IsDefault(const NodeId& value);
bool IsDefault(const ExpandedNodeId& value);
bool IsDefault(Status value);
bool IsDefault(const QualifiedName& value);
bool IsDefault(const LocalizedText& value);
bool IsDefault(const ExtensionObject& value);
bool IsDefault(const Variant& value);
bool IsDefault(const DataValue& value);
bool IsDefault(const DiagnosticInfo& value);

// Reads a required field, throwing if it is absent.
const boost::json::value& RequireField(const boost::json::object& object,
                                       std::string_view name);

}  // namespace opcua::ua::json
