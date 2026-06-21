#pragma once

#include "opcua/scada/basic_types.h"
#include "opcua/scada/expanded_node_id.h"
#include "opcua/scada/localized_text.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/qualified_name.h"
#include "opcua/scada/variant.h"

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
  void Encode(bool value);
  void Encode(std::int32_t value);
  void Encode(std::int64_t value);
  void Encode(double value);

  void Encode(std::string_view value);
  void Encode(const String& value);
  void Encode(const QualifiedName& value);
  void Encode(const LocalizedText& value);
  void Encode(DateTime value);
  void Encode(const ByteString& value);
  void Encode(const NodeId& node_id);
  void Encode(const ExpandedNodeId& node_id);
  void Encode(const Variant& value);
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
  bool Decode(bool& value);
  bool Decode(std::int32_t& value);
  bool Decode(std::int64_t& value);
  bool Decode(double& value);

  bool Decode(String& value);
  bool Decode(QualifiedName& value);
  bool Decode(LocalizedText& value);
  bool Decode(DateTime& value);
  bool Decode(ByteString& value);
  bool Decode(NodeId& id);
  bool Decode(ExpandedNodeId& id);
  bool Decode(Variant& value);
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
