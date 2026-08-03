#pragma once

#include "opcua/types/basic_types.h"
#include "opcua/types/data_value.h"
#include "opcua/types/diagnostic_info.h"
#include "opcua/types/expanded_node_id.h"
#include "opcua/types/guid.h"
#include "opcua/types/localized_text.h"
#include "opcua/types/node_id.h"
#include "opcua/types/qualified_name.h"
#include "opcua/types/status.h"
#include "opcua/types/variant.h"
#include "opcua/types/xml_element.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opcua::binary {

struct EncodedExtensionObject {
  std::uint32_t type_id = 0;
  std::vector<char> body;
};

struct DecodedExtensionObject {
  std::uint32_t type_id = 0;
  std::uint8_t encoding = 0;
  std::vector<char> body;
};

class Encoder {
 public:
  explicit Encoder(std::vector<char>& bytes) : bytes_{bytes} {}

  void Encode(std::uint8_t value);
  void Encode(std::uint16_t value);
  void Encode(std::uint32_t value);
  void Encode(std::uint64_t value);
  void Encode(bool value);
  void Encode(std::int32_t value);
  void Encode(std::int64_t value);
  void Encode(float value);
  void Encode(double value);

  void Encode(std::string_view value);
  void Encode(const String& value);
  void Encode(const QualifiedName& value);
  void Encode(const LocalizedText& value);
  void Encode(DateTime value);
  void Encode(const Guid& value);
  void Encode(const ByteString& value);
  void Encode(const XmlElement& value);
  void Encode(const NodeId& node_id);
  void Encode(const ExpandedNodeId& node_id);
  void Encode(Status value);
  void Encode(const DiagnosticInfo& value);
  void Encode(const DataValue& value);
  void Encode(const Variant& value);
  // Writes the type id, encoding byte and length-prefixed body of an
  // ExtensionObject (OPC UA Part 6 §5.2.2.15). `EncodedExtensionObject` is the
  // same thing for a caller that already has the body as bytes and an ns-0
  // numeric id.
  void Encode(const ExtensionObject& value);
  void Encode(const EncodedExtensionObject& value);

  std::vector<char>& bytes() { return bytes_; }

 private:
  std::vector<char>& bytes_;
};

class Decoder {
 public:
  explicit Decoder(std::span<const char> bytes) : bytes_{bytes} {}
  explicit Decoder(const std::vector<char>& bytes) : bytes_{bytes} {}

  bool Decode(std::uint8_t& value);
  bool Decode(std::uint16_t& value);
  bool Decode(std::uint32_t& value);
  bool Decode(std::uint64_t& value);
  bool Decode(bool& value);
  bool Decode(std::int32_t& value);
  bool Decode(std::int64_t& value);
  bool Decode(float& value);
  bool Decode(double& value);

  bool Decode(String& value);
  bool Decode(QualifiedName& value);
  bool Decode(LocalizedText& value);
  bool Decode(DateTime& value);
  bool Decode(Guid& value);
  bool Decode(ByteString& value);
  bool Decode(XmlElement& value);
  bool Decode(NodeId& id);
  bool Decode(ExpandedNodeId& id);
  bool Decode(Status& value);
  bool Decode(DiagnosticInfo& value);
  bool Decode(DataValue& value);
  bool Decode(Variant& value);
  // Reads an ExtensionObject, keeping its body as raw bytes — decoding that
  // body into a concrete type is the caller's business (see
  // FromExtensionObject in opcua/ua/ua_binary_codec.h). A body whose type id
  // this stack does not know round-trips unchanged.
  bool Decode(ExtensionObject& value);
  bool Decode(DecodedExtensionObject& value);

  std::size_t offset() const { return offset_; }
  bool consumed() const { return offset_ == bytes_.size(); }
  std::span<const char> remaining() const { return bytes_.subspan(offset_); }
  bool Skip(std::size_t count);

 private:
  std::span<const char> bytes_;
  std::size_t offset_ = 0;
};

void AppendMessage(Encoder& encoder,
                   std::uint32_t type_id,
                   std::span<const char> payload);
// The returned payload span aliases the decoder input storage, so its lifetime
// is bound to the lifetime of the bytes owned by the input decoder.
std::optional<std::pair<std::uint32_t, std::span<const char>>> ReadMessage(
    Decoder& decoder);

}  // namespace opcua::binary
