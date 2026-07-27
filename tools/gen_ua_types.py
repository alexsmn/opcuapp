#!/usr/bin/env python3
"""Build-time generator: emit the OPC UA type system as C++ from the OPC
Foundation's machine-readable schema (the single source of truth, vendored
under `schema/` — see schema/README.md).

Inputs (under --schema):
  * `Opc.Ua.Types.bsd`  — the binary type dictionary: every StructuredType and
                          EnumeratedType, with field order, `LengthField` for
                          arrays and `SwitchField` for optional fields.
  * `NodeIds.csv`       — every standard NodeId, including the
                          `*_Encoding_DefaultBinary` / `*_Encoding_DefaultJson`
                          ids that identify a message on the wire.
  * `StatusCode.csv`    — the standard StatusCodes and their values.

  * `opc.ua.services.jsonschema.json`
                        — the JSON encoding of the same model. Used as a
                          cross-check: it expresses inheritance with `allOf`
                          where the dictionary flattens it, and once that is
                          resolved every shared structure's field list must
                          match, or the build fails.

Outputs (under --out, all in namespace `opcua::ua`):
  ua_types.h, ua_encoding_ids.h, ua_status_codes.h, ua_binary_codec.{h,cpp},
  ua_json_codec.{h,cpp}

The 15 built-in structural types (NodeId, Variant, DataValue, DiagnosticInfo,
…) are NOT generated: they are hand-written in `opcua/types/` because they have
bit-packed encodings and hand-tuned storage. Everything else is derived, so a
wrong field or a wrong id is fixed by bumping the schema, never by editing the
output.

Output is deterministic (schema order is preserved, and the topological sort is
stable), so repeated builds produce identical files.
"""
import argparse
import csv
import os
import re
import xml.etree.ElementTree as ElementTree

OPC_NS = "{http://opcfoundation.org/BinarySchema/}"

# Structural types with a bit-packed or otherwise irregular encoding. Their C++
# form and codec live in opcua/types/ + opcua/transport/binary/codec_utils.cpp;
# generating them would mean generating the bit twiddling too, for 15 types
# that never change.
BUILT_IN_STRUCTS = {
    "XmlElement",
    "TwoByteNodeId",
    "FourByteNodeId",
    "NumericNodeId",
    "StringNodeId",
    "GuidNodeId",
    "ByteStringNodeId",
    "NodeId",
    "ExpandedNodeId",
    "DiagnosticInfo",
    "QualifiedName",
    "LocalizedText",
    "DataValue",
    "ExtensionObject",
    "Variant",
}

# NodeIdType describes how a NodeId is encoded, which is the hand-written
# codec's business; opcua::NodeIdType already models the identifier kinds.
BUILT_IN_ENUMS = {"NodeIdType"}

# Schema type name -> the C++ type in namespace `opcua`. `opc:Char` and
# `opc:CharArray` are the schema's spellings for a UTF-8 string.
BUILT_IN_TYPE_MAP = {
    "opc:Boolean": "Boolean",
    "opc:SByte": "Int8",
    "opc:Byte": "UInt8",
    "opc:Int16": "Int16",
    "opc:UInt16": "UInt16",
    "opc:Int32": "Int32",
    "opc:UInt32": "UInt32",
    "opc:Int64": "Int64",
    "opc:UInt64": "UInt64",
    "opc:Float": "Float",
    "opc:Double": "Double",
    "opc:String": "String",
    "opc:Char": "String",
    "opc:CharArray": "String",
    "opc:DateTime": "DateTime",
    "opc:Guid": "Guid",
    "opc:ByteString": "ByteString",
    "ua:XmlElement": "XmlElement",
    "ua:NodeId": "NodeId",
    "ua:ExpandedNodeId": "ExpandedNodeId",
    "ua:StatusCode": "Status",
    "ua:QualifiedName": "QualifiedName",
    "ua:LocalizedText": "LocalizedText",
    "ua:ExtensionObject": "ExtensionObject",
    "ua:DataValue": "DataValue",
    "ua:Variant": "Variant",
    "ua:DiagnosticInfo": "DiagnosticInfo",
}

# Types whose default-constructed value is not `{}`-initialisable to something
# meaningful, or that are cheaper to pass around by reference. Only used to
# decide whether a member gets an explicit initialiser.
SCALAR_DEFAULTS = {
    "Boolean": "false",
    "Int8": "0",
    "UInt8": "0",
    "Int16": "0",
    "UInt16": "0",
    "Int32": "0",
    "UInt32": "0",
    "Int64": "0",
    "UInt64": "0",
    "Float": "0",
    "Double": "0",
}

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "class", "const", "constexpr", "continue",
    "decltype", "default", "delete", "do", "double", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "nullptr", "operator", "or", "private", "protected", "public", "register",
    "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "template", "this", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
}

# Acronyms the schema spells in full caps; splitting them naively would give
# `namespace_u_r_i`.
ACRONYMS = ["URIs", "URI", "URLs", "URL", "XML", "JSON", "GUID", "UUID", "ID",
            "IDs", "SHA", "RSA", "AES", "TCP", "UDP", "IP", "MAC", "DNS",
            "UTC", "CRL", "CA", "PKI", "QoS", "UA"]


def to_snake_case(name):
    """`NodesToRead` -> `nodes_to_read`, `NamespaceURI` -> `namespace_uri`."""
    for acronym in ACRONYMS:
        name = name.replace(acronym, acronym.capitalize())
    name = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    name = re.sub(r"_+", "_", name).lower()
    return name + "_" if name in CPP_KEYWORDS else name


class Enum:
    def __init__(self, name, bits, values, documentation):
        self.name = name
        self.bits = bits
        self.values = values  # list of (name, value)
        self.documentation = documentation

    @property
    def underlying(self):
        # LengthInBits is the width on the wire; anything narrower than 32 bits
        # still travels as its declared width, so the C++ type mirrors it.
        return {8: "UInt8", 16: "UInt16"}.get(self.bits, "Int32")


class Field:
    def __init__(self, name, type_name, is_array):
        self.name = name
        self.type_name = type_name  # C++ type, without vector
        self.is_array = is_array
        self.member = to_snake_case(name)

    def declaration(self, known_enums):
        if self.is_array:
            return "std::vector<%s> %s;" % (self.type_name, self.member)
        default = SCALAR_DEFAULTS.get(self.type_name)
        if default is not None:
            return "%s %s = %s;" % (self.type_name, self.member, default)
        if self.type_name in known_enums:
            return "%s %s{};" % (self.type_name, self.member)
        return "%s %s;" % (self.type_name, self.member)


class Struct:
    def __init__(self, name, fields, documentation):
        self.name = name
        self.fields = fields
        self.documentation = documentation

    def dependencies(self):
        """Generated types this struct needs the definition of.

        An array member only needs the element type to be complete at the point
        of use, but emitting in dependency order keeps the header readable and
        catches genuine cycles.
        """
        return {field.type_name for field in self.fields}


def parse_schema(path):
    tree = ElementTree.parse(path)
    root = tree.getroot()

    enums = []
    for node in root.findall(OPC_NS + "EnumeratedType"):
        name = node.get("Name")
        if name in BUILT_IN_ENUMS:
            continue
        values = [(value.get("Name"), int(value.get("Value")))
                  for value in node.findall(OPC_NS + "EnumeratedValue")]
        enums.append(Enum(name, int(node.get("LengthInBits", "32")), values,
                          documentation(node)))

    structs = []
    for node in root.findall(OPC_NS + "StructuredType"):
        name = node.get("Name")
        if name in BUILT_IN_STRUCTS:
            continue
        structs.append(Struct(name, parse_fields(node, name),
                              documentation(node)))
    return enums, structs


def documentation(node):
    element = node.find(OPC_NS + "Documentation")
    if element is None or not element.text:
        return ""
    return " ".join(element.text.split())


def parse_fields(node, struct_name):
    elements = node.findall(OPC_NS + "Field")
    # `NoOfXxx` length fields are an artifact of the binary encoding; the array
    # they size becomes a std::vector, so they are not members.
    length_fields = {element.get("LengthField")
                     for element in elements
                     if element.get("LengthField")}

    fields = []
    for element in elements:
        name = element.get("Name")
        if name in length_fields:
            continue
        type_name = element.get("TypeName")
        if type_name == "opc:Bit":
            raise SystemExit(
                "%s.%s is a bit field; only the hand-written built-in types "
                "have those. Either the schema changed shape or %s belongs in "
                "BUILT_IN_STRUCTS." % (struct_name, name, struct_name))
        if element.get("SwitchField"):
            raise SystemExit(
                "%s.%s is an optional field (SwitchField=%s); the generator "
                "has no encoding-mask support yet." %
                (struct_name, name, element.get("SwitchField")))
        fields.append(Field(name, map_type(type_name, struct_name, name),
                            element.get("LengthField") is not None))
    return fields


def map_type(type_name, struct_name, field_name):
    if type_name in BUILT_IN_TYPE_MAP:
        return BUILT_IN_TYPE_MAP[type_name]
    if type_name.startswith("tns:"):
        return type_name[4:]
    raise SystemExit("unmapped schema type %s (on %s.%s)" %
                     (type_name, struct_name, field_name))


def sort_structs(structs):
    """Orders structs so that every struct follows the ones it contains.

    Stable: ties keep schema order, so the output does not churn between runs.
    """
    by_name = {struct.name: struct for struct in structs}
    ordered = []
    emitted = set()
    visiting = set()

    def visit(struct, stack):
        if struct.name in emitted:
            return
        if struct.name in visiting:
            raise SystemExit("cycle in schema struct references: %s" %
                             " -> ".join(stack + [struct.name]))
        visiting.add(struct.name)
        for dependency in sorted(struct.dependencies()):
            if dependency in by_name:
                visit(by_name[dependency], stack + [struct.name])
        visiting.discard(struct.name)
        emitted.add(struct.name)
        ordered.append(struct)

    for struct in structs:
        visit(struct, [])
    return ordered


def wrap_comment(text, indent=""):
    if not text:
        return []
    lines = []
    current = indent + "//"
    for word in text.split():
        if len(current) + 1 + len(word) > 80:
            lines.append(current)
            current = indent + "//"
        current += " " + word
    lines.append(current)
    return lines


HEADER_PREAMBLE = """// GENERATED FILE — DO NOT EDIT.
//
// Produced by tools/gen_ua_types.py from the vendored OPC UA schema
// (schema/%s, pinned in schema/VERSION). To change anything
// here, bump the schema and rebuild; see schema/README.md.
"""


def write_types_header(path, enums, structs, node_ids):
    known_enums = {enum.name for enum in enums}
    out = [HEADER_PREAMBLE % "Opc.Ua.Types.bsd"]
    out.append("#pragma once\n")
    out.append('#include "opcua/types/basic_types.h"')
    out.append('#include "opcua/types/data_value.h"')
    out.append('#include "opcua/types/diagnostic_info.h"')
    out.append('#include "opcua/types/expanded_node_id.h"')
    out.append('#include "opcua/types/extension_object.h"')
    out.append('#include "opcua/types/guid.h"')
    out.append('#include "opcua/types/localized_text.h"')
    out.append('#include "opcua/types/node_id.h"')
    out.append('#include "opcua/types/qualified_name.h"')
    out.append('#include "opcua/types/status.h"')
    out.append('#include "opcua/types/string.h"')
    out.append('#include "opcua/types/variant.h"')
    out.append('#include "opcua/types/xml_element.h"\n')
    out.append("#include <cstdint>")
    out.append("#include <string_view>")
    out.append("#include <vector>\n")
    out.append("// The OPC UA type system as defined by OPC UA Part 3 (Address")
    out.append("// Space Model) and Part 4 (Services), transcribed by the OPC")
    out.append("// Foundation into Opc.Ua.Types.bsd. The built-in types these")
    out.append("// build on live in opcua/types/ and are hand-written.")
    out.append("namespace opcua::ua {\n")

    for enum in enums:
        out.extend(wrap_comment(enum.documentation))
        out.append("enum class %s : %s {" % (enum.name, enum.underlying))
        for name, value in enum.values:
            out.append("  %s = %d," % (name, value))
        out.append("};\n")

    for struct in structs:
        out.extend(wrap_comment(struct.documentation))
        members = []
        # The DefaultBinary encoding id (the ns-0 NodeId that identifies this
        # type on the wire) lives with the type as the source of truth for
        # BinaryEncodingId<T> and the service dispatch tables. A static member
        # does not participate in aggregate initialization, so designated
        # initializers are unaffected.
        encoding_id = node_ids.get(struct.name + "_Encoding_DefaultBinary")
        if encoding_id is not None:
            members.append(
                "  static constexpr std::uint32_t kBinaryEncodingId = %d;"
                % encoding_id)
        # The DefaultJson encoding id, for the same purpose on the UA-JSON
        # transport: an ExtensionObject carrying a JSON body identifies its
        # type with this id, not the binary one (Part 6 §5.4.2.16).
        json_encoding_id = node_ids.get(struct.name + "_Encoding_DefaultJson")
        if json_encoding_id is not None:
            members.append(
                "  static constexpr std::uint32_t kJsonEncodingId = %d;"
                % json_encoding_id)
        if encoding_id is not None:
            # The service-operation name a request/response travels under in the
            # JSON envelope (the type name minus its Request/Response suffix,
            # e.g. ReadRequest/ReadResponse -> "Read"). Emitted for every wire
            # message so the websocket dispatch keys off the type rather than a
            # hand-written RequestServiceName<T>.
            for suffix in ("Request", "Response"):
                if struct.name.endswith(suffix):
                    members.append(
                        '  static constexpr std::string_view kServiceName = '
                        '"%s";' % struct.name[:-len(suffix)])
                    break
        for field in struct.fields:
            members.append("  " + field.declaration(known_enums))
        if not members:
            # An abstract base with no fields of its own and no wire encoding;
            # concrete subtypes repeat its fields, so it carries no data.
            out.append("struct %s {};\n" % struct.name)
            continue
        out.append("struct %s {" % struct.name)
        out.extend(members)
        out.append("};\n")

    out.append("}  // namespace opcua::ua")
    write(path, "\n".join(out) + "\n")


# The built-in types whose scalar codec is a plain Encoder::Encode /
# Decoder::Decode call. Int8 and Int16 are handled separately: they share their
# unsigned counterpart's wire form and have no overload of their own.
DIRECT_BUILT_INS = [
    "Boolean", "UInt8", "UInt16", "Int32", "UInt32", "Int64", "UInt64",
    "Float", "Double", "String", "DateTime", "Guid", "ByteString",
    "XmlElement", "NodeId", "ExpandedNodeId", "Status", "QualifiedName",
    "LocalizedText", "ExtensionObject", "DataValue", "Variant",
    "DiagnosticInfo",
]

CODEC_HEADER_PRELUDE = '''#pragma once

#include "opcua/transport/binary/codec_utils.h"
#include "opcua/ua/ua_encoding_ids.h"
#include "opcua/ua/ua_types.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

// The OPC UA Binary encoding of every generated type (OPC UA Part 6 §5.2,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2). Structures
// are a plain sequence of their fields, and arrays are an Int32 element count
// followed by the elements — the schema pins the field order, so nothing here
// is a judgement call. The built-in types these bottom out in are hand-written
// (opcua/transport/binary/codec_utils.cpp).
namespace opcua::ua {

// The DefaultBinary encoding id of a generated type, i.e. the NodeId that
// identifies it on the wire. Reads the type's own kBinaryEncodingId member
// (the generator emits it on every type that has a wire encoding); ill-formed
// for a type that has none.
template <class T>
struct BinaryEncodingId {
  static constexpr std::uint32_t value = T::kBinaryEncodingId;
};

namespace detail {

// Scalar codec. The overloads below cover the hand-written built-ins; the
// trailing template picks up the generated structs and enums through their
// Encode/Decode overloads.
inline void EncodeValue(binary::Encoder& encoder, Int8 value) {
  encoder.Encode(static_cast<UInt8>(value));
}

inline void EncodeValue(binary::Encoder& encoder, Int16 value) {
  encoder.Encode(static_cast<UInt16>(value));
}

inline bool DecodeValue(binary::Decoder& decoder, Int8& value) {
  UInt8 raw = 0;
  if (!decoder.Decode(raw))
    return false;
  value = static_cast<Int8>(raw);
  return true;
}

inline bool DecodeValue(binary::Decoder& decoder, Int16& value) {
  UInt16 raw = 0;
  if (!decoder.Decode(raw))
    return false;
  value = static_cast<Int16>(raw);
  return true;
}
'''

CODEC_HEADER_HELPERS = '''
template <class T>
void EncodeValue(binary::Encoder& encoder, const T& value) {
  Encode(encoder, value);
}

template <class T>
bool DecodeValue(binary::Decoder& decoder, T& value) {
  return Decode(decoder, value);
}

// OPC UA Part 6 §5.2.5 Arrays: an Int32 element count followed by the
// elements. A count of -1 means a null array, which is indistinguishable from
// an empty one here and decodes to an empty vector.
template <class T>
void EncodeArray(binary::Encoder& encoder, const std::vector<T>& values) {
  encoder.Encode(static_cast<Int32>(values.size()));
  for (const T& value : values)
    EncodeValue(encoder, value);
}

template <class T>
bool DecodeArray(binary::Decoder& decoder, std::vector<T>& values) {
  Int32 count = 0;
  if (!decoder.Decode(count))
    return false;
  values.clear();
  if (count <= 0)
    return true;
  // Every element occupies at least one byte, so an array cannot hold more
  // elements than there are bytes left. Rejecting a larger count bounds the
  // reservation against a malformed or hostile length (decode bomb). OPC UA
  // Part 6 §5.1.2 Decoding Errors,
  // https://reference.opcfoundation.org/Core/Part6/v105/docs/5.1.2
  if (static_cast<std::size_t>(count) > decoder.remaining().size())
    return false;
  values.resize(static_cast<std::size_t>(count));
  for (std::size_t index = 0; index < values.size(); ++index) {
    // Indexed rather than range-based: std::vector<bool> hands out a proxy
    // reference that will not bind to `T&`.
    T element{};
    if (!DecodeValue(decoder, element))
      return false;
    values[index] = std::move(element);
  }
  return true;
}

}  // namespace detail

// Wraps a value as an ExtensionObject carrying its DefaultBinary encoding, the
// form structured values take inside a Variant or an `ua:ExtensionObject`
// field.
template <class T>
ExtensionObject ToExtensionObject(const T& value) {
  ByteString body;
  binary::Encoder encoder{body};
  Encode(encoder, value);
  return ExtensionObject{ExpandedNodeId{NodeId{BinaryEncodingId<T>::value}},
                         std::move(body)};
}

// The inverse. Returns false when the ExtensionObject carries a different type
// id, has no binary body, or the body does not decode cleanly.
template <class T>
bool FromExtensionObject(const ExtensionObject& extension_object, T& value) {
  const ExpandedNodeId& id = extension_object.data_type_id();
  if (!id.node_id().is_numeric() || id.node_id().namespace_index() != 0 ||
      id.node_id().numeric_id() != BinaryEncodingId<T>::value) {
    return false;
  }
  const ByteString* body = extension_object.binary_body();
  if (body == nullptr)
    return false;
  binary::Decoder decoder{*body};
  return Decode(decoder, value) && decoder.consumed();
}
'''


def write_binary_codec(header_path, source_paths, enums, structs, node_ids):
    """Emits Encode/Decode for every generated enum and struct.

    Definitions are split across `source_paths` so no single translation unit
    has to hold all 357 structs' worth of codec.
    """
    out = [HEADER_PREAMBLE % "Opc.Ua.Types.bsd"]
    out.append(CODEC_HEADER_PRELUDE)
    for name in DIRECT_BUILT_INS:
        out.append("""
inline void EncodeValue(binary::Encoder& encoder, const %s& value) {
  encoder.Encode(value);
}

inline bool DecodeValue(binary::Decoder& decoder, %s& value) {
  return decoder.Decode(value);
}""" % (name, name))
    out.append("\n}  // namespace detail\n")
    out.append("// Enumerations travel as their underlying integer.")
    for enum in enums:
        out.append("void Encode(binary::Encoder& encoder, %s value);" %
                   enum.name)
        out.append("bool Decode(binary::Decoder& decoder, %s& value);" %
                   enum.name)
    out.append("")
    for struct in structs:
        out.append("void Encode(binary::Encoder& encoder, const %s& value);" %
                   struct.name)
        out.append("bool Decode(binary::Decoder& decoder, %s& value);" %
                   struct.name)
    out.append("")
    out.append("namespace detail {")
    out.append(CODEC_HEADER_HELPERS.strip())
    out.append("")
    out.append("}  // namespace opcua::ua")
    write(header_path, "\n".join(out) + "\n")

    shards = [[] for _ in source_paths]
    for index, struct in enumerate(structs):
        shards[index % len(source_paths)].append(struct)

    for index, (path, shard) in enumerate(zip(source_paths, shards)):
        source = [HEADER_PREAMBLE % "Opc.Ua.Types.bsd"]
        source.append('#include "opcua/ua/ua_binary_codec.h"\n')
        source.append("namespace opcua::ua {\n")
        if index == 0:
            for enum in enums:
                source.append(
                    "void Encode(binary::Encoder& encoder, %s value) {\n"
                    "  detail::EncodeValue(encoder, static_cast<%s>(value));\n"
                    "}\n" % (enum.name, enum.underlying))
                source.append(
                    "bool Decode(binary::Decoder& decoder, %s& value) {\n"
                    "  %s raw = 0;\n"
                    "  if (!detail::DecodeValue(decoder, raw))\n"
                    "    return false;\n"
                    "  value = static_cast<%s>(raw);\n"
                    "  return true;\n"
                    "}\n" % (enum.name, enum.underlying, enum.name))
        for struct in shard:
            source.append(encode_definition(struct))
            source.append(decode_definition(struct))
        source.append("}  // namespace opcua::ua")
        write(path, "\n".join(source) + "\n")


def encode_definition(struct):
    lines = ["void Encode(binary::Encoder& encoder, const %s& value) {" %
             struct.name]
    if not struct.fields:
        lines.append("  (void)encoder;")
        lines.append("  (void)value;")
    for field in struct.fields:
        if field.is_array:
            lines.append("  detail::EncodeArray(encoder, value.%s);" %
                         field.member)
        else:
            lines.append("  detail::EncodeValue(encoder, value.%s);" %
                         field.member)
    lines.append("}\n")
    return "\n".join(lines)


def decode_definition(struct):
    lines = ["bool Decode(binary::Decoder& decoder, %s& value) {" % struct.name]
    if not struct.fields:
        lines.append("  (void)decoder;")
        lines.append("  (void)value;")
    for field in struct.fields:
        helper = "DecodeArray" if field.is_array else "DecodeValue"
        lines.append("  if (!detail::%s(decoder, value.%s))" %
                     (helper, field.member))
        lines.append("    return false;")
    lines.append("  return true;")
    lines.append("}\n")
    return "\n".join(lines)


JSON_HEADER_PRELUDE = '''#pragma once

#include "opcua/ua/ua_json_builtins.h"
#include "opcua/ua/ua_types.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <any>
#include <cstdint>
#include <vector>

// The OPC UA JSON encoding of every generated type, in the compact form the
// OPC Foundation's published service schema describes (OPC UA Part 6 §5.4,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4). Field names
// are the spec's, taken from the same schema the C++ members are derived from
// — the generator checks them against schema/opc.ua.services.jsonschema.json,
// so the two OPC Foundation artifacts have to agree before this compiles.
//
// A field is omitted when it holds its default. Nested structures are always
// emitted, since "default" is not a question this layer can answer for them
// cheaply; a decoder treats an absent field as its default either way.
namespace opcua::ua {

namespace detail {

// Dispatches to the hand-written built-in codec where one exists, and to the
// generated per-type overloads otherwise.
template <class T>
boost::json::value EncodeJsonValue(const T& value) {
  if constexpr (requires { json::Encode(value); })
    return json::Encode(value);
  else
    return EncodeJson(value);
}

template <class T>
void DecodeJsonValue(const boost::json::value& source, T& value) {
  if constexpr (requires { json::Decode(source, value); })
    json::Decode(source, value);
  else
    DecodeJson(source, value);
}

template <class T>
boost::json::value EncodeJsonArray(const std::vector<T>& values) {
  boost::json::array result;
  result.reserve(values.size());
  for (const T& value : values)
    result.emplace_back(EncodeJsonValue(value));
  return result;
}

template <class T>
void DecodeJsonArray(const boost::json::value& source,
                     std::vector<T>& values) {
  values.clear();
  // A null array and an absent one are both the empty array here.
  if (source.is_null())
    return;
  if (!source.is_array())
    json::ThrowError("expected a JSON array");
  const boost::json::array& items = source.as_array();
  values.resize(items.size());
  for (std::size_t index = 0; index < items.size(); ++index) {
    // Indexed rather than range-based: std::vector<bool> hands out a proxy.
    T element{};
    DecodeJsonValue(items[index], element);
    values[index] = std::move(element);
  }
}

}  // namespace detail
'''


JSON_EXTENSION_OBJECT_HELPERS = """// --- ExtensionObject helpers, JSON flavour ---------------------------------
//
// The binary transport's ua::ToExtensionObject / ua::FromExtensionObject
// (ua_binary_codec.h) wrap a structure in an ExtensionObject whose body is a
// binary ByteString keyed by the DefaultBinary encoding id. On the UA-JSON
// transport the body is JSON and the id is DefaultJson (Part 6 §5.4.2.16,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.16), so the
// binary helpers cannot read a JSON-bodied ExtensionObject at all — they
// require binary_body() and report every JSON body as a type mismatch. These
// are their counterparts.

// The DefaultJson encoding id of a generated type. Reads the type's own
// kJsonEncodingId member (the generator emits it for every type that has one);
// ill-formed for a type that has none.
template <class T>
struct JsonEncodingId {
  static constexpr std::uint32_t value = T::kJsonEncodingId;
};

// Wrap `value` in an ExtensionObject carrying its JSON encoding.
template <class T>
ExtensionObject ToJsonExtensionObject(const T& value) {
  return ExtensionObject{ExpandedNodeId{NodeId{JsonEncodingId<T>::value}},
                         EncodeJson(value)};
}

// The inverse. Returns false when the ExtensionObject names a different type,
// carries no JSON body, or the body does not decode cleanly.
//
// The DefaultBinary id is accepted alongside the DefaultJson one: a peer that
// names a JSON-bodied structure by its binary encoding id is still unambiguous
// about which structure it means, and both ids appear in the wild. The body
// itself must be JSON — an ExtensionObject with a binary body belongs to
// ua::FromExtensionObject.
template <class T>
bool FromJsonExtensionObject(const ExtensionObject& extension_object,
                             T& value) {
  const ExpandedNodeId& id = extension_object.data_type_id();
  if (!id.node_id().is_numeric() || id.node_id().namespace_index() != 0)
    return false;
  const std::uint32_t numeric_id = id.node_id().numeric_id();
  bool id_matches = numeric_id == JsonEncodingId<T>::value;
  if constexpr (requires { T::kBinaryEncodingId; })
    id_matches = id_matches || numeric_id == T::kBinaryEncodingId;
  if (!id_matches)
    return false;
  const auto* body =
      std::any_cast<boost::json::value>(&extension_object.value());
  if (body == nullptr)
    return false;
  // A malformed body is a peer error, not ours: report it as a type mismatch,
  // the same way the binary helper reports an undecodable ByteString.
  try {
    DecodeJson(*body, value);
  } catch (...) {
    return false;
  }
  return true;
}
"""


def write_json_codec(header_path, source_paths, enums, structs):
    out = [HEADER_PREAMBLE % "Opc.Ua.Types.bsd"]
    out.append(JSON_HEADER_PRELUDE)
    out.append("// Enumerations are JSON integers.")
    for enum in enums:
        out.append("boost::json::value EncodeJson(%s value);" % enum.name)
        out.append("void DecodeJson(const boost::json::value& source, "
                   "%s& value);" % enum.name)
    out.append("")
    for struct in structs:
        out.append("boost::json::value EncodeJson(const %s& value);" %
                   struct.name)
        out.append("void DecodeJson(const boost::json::value& source, "
                   "%s& value);" % struct.name)
    out.append("")
    out.append(JSON_EXTENSION_OBJECT_HELPERS)
    out.append("}  // namespace opcua::ua")
    write(header_path, "\n".join(out) + "\n")

    shards = [[] for _ in source_paths]
    for index, struct in enumerate(structs):
        shards[index % len(source_paths)].append(struct)

    for index, (path, shard) in enumerate(zip(source_paths, shards)):
        source = [HEADER_PREAMBLE % "Opc.Ua.Types.bsd"]
        source.append('#include "opcua/ua/ua_json_codec.h"\n')
        source.append("namespace opcua::ua {\n")
        if index == 0:
            for enum in enums:
                source.append(
                    "boost::json::value EncodeJson(%s value) {\n"
                    "  return json::Encode(static_cast<%s>(value));\n"
                    "}\n" % (enum.name, enum.underlying))
                source.append(
                    "void DecodeJson(const boost::json::value& source, "
                    "%s& value) {\n"
                    "  %s raw = 0;\n"
                    "  json::Decode(source, raw);\n"
                    "  value = static_cast<%s>(raw);\n"
                    "}\n" % (enum.name, enum.underlying, enum.name))
        for struct in shard:
            source.append(encode_json_definition(struct))
            source.append(decode_json_definition(struct))
        source.append("}  // namespace opcua::ua")
        write(path, "\n".join(source) + "\n")


def encode_json_definition(struct):
    lines = ["boost::json::value EncodeJson(const %s& value) {" % struct.name,
             "  boost::json::object json;"]
    for field in struct.fields:
        if field.is_array:
            lines.append("  if (!value.%s.empty())" % field.member)
            lines.append("    json[\"%s\"] = detail::EncodeJsonArray(value.%s);"
                         % (field.name, field.member))
        elif field.type_name in BUILT_IN_TYPE_MAP.values():
            # Built-in scalars carry an IsDefault overload, so they can be
            # omitted when unset.
            lines.append("  if (!json::IsDefault(value.%s))" % field.member)
            lines.append("    json[\"%s\"] = json::Encode(value.%s);"
                         % (field.name, field.member))
        else:
            lines.append("  json[\"%s\"] = detail::EncodeJsonValue(value.%s);"
                         % (field.name, field.member))
    lines.append("  return json;")
    lines.append("}\n")
    return "\n".join(lines)


def decode_json_definition(struct):
    lines = ["void DecodeJson(const boost::json::value& source, %s& value) {" %
             struct.name]
    if not struct.fields:
        lines.append("  (void)source;")
        lines.append("  (void)value;")
        lines.append("}\n")
        return "\n".join(lines)
    lines.append("  if (!source.is_object())")
    lines.append("    json::ThrowError(\"expected a JSON object\");")
    lines.append("  const boost::json::object& json = source.as_object();")
    lines.append("  value = %s{};" % struct.name)
    for field in struct.fields:
        helper = "DecodeJsonArray" if field.is_array else "DecodeJsonValue"
        lines.append("  if (const boost::json::value* field = "
                     "json.if_contains(\"%s\"))" % field.name)
        lines.append("    detail::%s(*field, value.%s);" %
                     (helper, field.member))
    lines.append("}\n")
    return "\n".join(lines)


def check_json_schema(path, structs):
    """Cross-checks the field names against the published JSON schema.

    The binary dictionary and the JSON schema are separately published
    descriptions of the same model: the dictionary flattens inheritance while
    the JSON schema expresses it with `allOf`. Resolving that, every shared
    structure's field list matches exactly — which is what lets the generated
    JSON codec use the dictionary's field names verbatim. If a schema bump ever
    breaks the correspondence, fail the build rather than emit JSON with the
    wrong property names.
    """
    with open(path, encoding="utf-8") as handle:
        definitions = __import__("json").load(handle)["$defs"]

    def resolved_properties(name):
        node = definitions[name]
        properties = []
        for base in node.get("allOf", []):
            properties += resolved_properties(base["$ref"].rsplit("/", 1)[-1])
        return properties + list(node.get("properties", {}).keys())

    mismatches = []
    for struct in structs:
        if struct.name not in definitions:
            # The JSON schema omits a handful of types (the ThreeD* geometry
            # aliases); nothing to check.
            continue
        expected = resolved_properties(struct.name)
        actual = [field.name for field in struct.fields]
        if expected != actual:
            mismatches.append("%s: binary dictionary %s, JSON schema %s" %
                              (struct.name, actual, expected))
    if mismatches:
        raise SystemExit(
            "the vendored binary dictionary and JSON schema disagree:\n  " +
            "\n  ".join(mismatches))


def read_node_ids(path):
    """Returns {symbolic name: numeric id} for every ns-0 Node."""
    ids = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if len(row) >= 2:
                ids[row[0]] = int(row[1])
    return ids


def write_encoding_ids_header(path, node_ids):
    """Emits the DefaultBinary/DefaultJson encoding ids.

    These identify a message or an ExtensionObject body on the wire. They are
    NOT the DataType ids — for a service they differ by two, and using the
    wrong one makes every third-party client report an unsupported service.
    """
    binary = sorted((name[: -len("_Encoding_DefaultBinary")], value)
                    for name, value in node_ids.items()
                    if name.endswith("_Encoding_DefaultBinary"))
    json_ids = sorted((name[: -len("_Encoding_DefaultJson")], value)
                      for name, value in node_ids.items()
                      if name.endswith("_Encoding_DefaultJson"))

    out = [HEADER_PREAMBLE % "NodeIds.csv"]
    out.append("#pragma once\n")
    out.append("#include <cstdint>\n")
    out.append("// NodeIds of the DataTypeEncoding Nodes: the id that")
    out.append("// identifies a message on the wire (OPC UA Part 6 §5.2.2.15")
    out.append("// and Annex A,")
    out.append("// https://reference.opcfoundation.org/Core/Part6/v105/docs/A.1).")
    out.append("// These are NOT DataType ids — for a service the two differ,")
    out.append("// and using the DataType id makes conforming clients report")
    out.append("// an unsupported service.")
    out.append("namespace opcua::ua {\n")

    out.append("// Default Binary encoding ids.")
    out.append("namespace binary_encoding_id {\n")
    for name, value in binary:
        out.append("constexpr std::uint32_t k%s = %d;" % (name, value))
    out.append("\n}  // namespace binary_encoding_id\n")

    out.append("// Default JSON encoding ids.")
    out.append("namespace json_encoding_id {\n")
    for name, value in json_ids:
        out.append("constexpr std::uint32_t k%s = %d;" % (name, value))
    out.append("\n}  // namespace json_encoding_id\n")

    out.append("}  // namespace opcua::ua")
    write(path, "\n".join(out) + "\n")


def write_status_codes_header(path, schema_dir):
    out = [HEADER_PREAMBLE % "StatusCode.csv"]
    out.append("#pragma once\n")
    out.append("#include <cstdint>\n")
    out.append("// The standard StatusCodes as full 32-bit values (OPC UA")
    out.append("// Part 4 §7.38 StatusCode,")
    out.append("// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38).")
    out.append("// opcua::StatusCode holds opcuapp's own enumeration, which")
    out.append("// mixes these with vendor codes; these constants are the")
    out.append("// unedited standard values.")
    out.append("namespace opcua::ua::status_code {\n")
    with open(os.path.join(schema_dir, "StatusCode.csv"), newline="",
              encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if len(row) >= 2:
                out.append("constexpr std::uint32_t k%s = %s;" %
                           (row[0], row[1]))
    out.append("\n}  // namespace opcua::ua::status_code")
    write(path, "\n".join(out) + "\n")


def write(path, content):
    # Only rewrite on change, so an unchanged schema does not invalidate every
    # dependent object file.
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            if handle.read() == content:
                return
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument(
        "--codec-shards", type=int, default=4,
        help="translation units to split the generated codec across; must "
             "match the count the build system expects")
    args = parser.parse_args()

    enums, structs = parse_schema(os.path.join(args.schema,
                                               "Opc.Ua.Types.bsd"))
    structs = sort_structs(structs)
    node_ids = read_node_ids(os.path.join(args.schema, "NodeIds.csv"))

    os.makedirs(args.out, exist_ok=True)
    write_types_header(os.path.join(args.out, "ua_types.h"), enums, structs,
                       node_ids)
    write_encoding_ids_header(os.path.join(args.out, "ua_encoding_ids.h"),
                              node_ids)
    write_status_codes_header(os.path.join(args.out, "ua_status_codes.h"),
                              args.schema)
    check_json_schema(
        os.path.join(args.schema, "opc.ua.services.jsonschema.json"), structs)
    write_binary_codec(
        os.path.join(args.out, "ua_binary_codec.h"),
        [os.path.join(args.out, "ua_binary_codec_%d.cpp" % index)
         for index in range(args.codec_shards)],
        enums, structs, node_ids)
    write_json_codec(
        os.path.join(args.out, "ua_json_codec.h"),
        [os.path.join(args.out, "ua_json_codec_%d.cpp" % index)
         for index in range(args.codec_shards)],
        enums, structs)


if __name__ == "__main__":
    main()
