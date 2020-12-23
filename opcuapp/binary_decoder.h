#pragma once

#include <opcua.h>
#include <opcua_binaryencoder.h>
#include <opcuapp/basic_types.h>
#include <opcuapp/expanded_node_id.h>
#include <opcuapp/extension_object.h>
#include <opcuapp/localized_text.h>
#include <opcuapp/node_id.h>
#include <opcuapp/qualified_name.h>
#include <opcuapp/status_code.h>
#include <opcuapp/structs.h>
#include <opcuapp/variant.h>
#include <unordered_map>
#include <vector>

namespace opcua {

#define BINARY_DECODER_READ(DecodeType)                                   \
  template <>                                                             \
  inline DecodeType BinaryDecoder::Read(OpcUa_StringA field_name) const { \
    OpcUa_##DecodeType value;                                             \
    Check(decoder_->Read##DecodeType(                                     \
        reinterpret_cast<OpcUa_Decoder*>(decode_context_), field_name,    \
        &value));                                                         \
    return value;                                                         \
  }

class BinaryDecoder {
 public:
  BinaryDecoder() { Check(::OpcUa_BinaryDecoder_Create(&decoder_)); }

  ~BinaryDecoder() {
    if (decode_context_)
      decoder_->Close(decoder_, &decode_context_);
    ::OpcUa_Decoder_Delete(&decoder_);
  }

  BinaryDecoder(const BinaryDecoder&) = delete;
  BinaryDecoder& operator=(const BinaryDecoder&) = delete;

  void Open(OpcUa_InputStream& stream, OpcUa_MessageContext& context) {
    Check(decoder_->Open(decoder_, &stream, &context, &decode_context_));
  }

  void Close() { Check(decoder_->Close(decoder_, &decode_context_)); }

  using NamespaceMapping = std::unordered_map<NamespaceIndex, NamespaceIndex>;

  void set_namespace_mapping(NamespaceMapping mapping) {
    namespace_mapping_ = std::move(mapping);
  }

  template <typename T>
  T Read(OpcUa_StringA field_name = nullptr) const;

  template <typename T>
  T ReadEnum(OpcUa_StringA field_name = nullptr) const;

  template <typename T>
  std::vector<T> ReadArray() const;

  void ReadEncodable(const OpcUa_EncodeableType& type,
                     OpcUa_Void* object,
                     OpcUa_StringA field_name = nullptr);

 private:
  NamespaceIndex MapNamespaceIndex(NamespaceIndex namespace_index) const;

  OpcUa_Decoder* decoder_ = OpcUa_Null;
  OpcUa_Handle decode_context_ = OpcUa_Null;
  NamespaceMapping namespace_mapping_;
};

BINARY_DECODER_READ(Boolean);
BINARY_DECODER_READ(SByte);
BINARY_DECODER_READ(Int16);
BINARY_DECODER_READ(UInt16);
BINARY_DECODER_READ(Int32);
BINARY_DECODER_READ(UInt32);
BINARY_DECODER_READ(Double);
BINARY_DECODER_READ(String);
BINARY_DECODER_READ(ByteString);
BINARY_DECODER_READ(StatusCode);
BINARY_DECODER_READ(Variant);
BINARY_DECODER_READ(QualifiedName);
BINARY_DECODER_READ(LocalizedText);
BINARY_DECODER_READ(ExtensionObject);

template <>
inline NodeId BinaryDecoder::Read(OpcUa_StringA field_name) const {
  OpcUa_NodeId value;
  Check(decoder_->ReadNodeId(reinterpret_cast<OpcUa_Decoder*>(decode_context_),
                             field_name, &value));
  value.NamespaceIndex = MapNamespaceIndex(value.NamespaceIndex);
  return value;
}

template <>
inline ExpandedNodeId BinaryDecoder::Read(OpcUa_StringA field_name) const {
  OpcUa_ExpandedNodeId value;
  Check(decoder_->ReadExpandedNodeId(
      reinterpret_cast<OpcUa_Decoder*>(decode_context_), field_name, &value));
  value.NodeId.NamespaceIndex = MapNamespaceIndex(value.NodeId.NamespaceIndex);
  return value;
}

template <typename T>
inline T BinaryDecoder::ReadEnum(OpcUa_StringA field_name) const {
  return static_cast<T>(Read<Int32>(field_name));
}

template <typename T>
inline std::vector<T> BinaryDecoder::ReadArray() const {
  auto len = Read<Int32>();
  if (len == -1)
    return {};
  std::vector<T> array(static_cast<size_t>(len));
  for (auto& v : array)
    v = Read<T>();
  return array;
}

inline void BinaryDecoder::ReadEncodable(const OpcUa_EncodeableType& type,
                                         OpcUa_Void* object,
                                         OpcUa_StringA field_name) {
  Check(decoder_->ReadEncodeable(
      reinterpret_cast<OpcUa_Decoder*>(decode_context_), field_name,
      &const_cast<OpcUa_EncodeableType&>(type), object));
}

inline NamespaceIndex BinaryDecoder::MapNamespaceIndex(
    NamespaceIndex namespace_index) const {
  auto i = namespace_mapping_.find(namespace_index);
  return i == namespace_mapping_.end() ? namespace_index : i->second;
}

}  // namespace opcua
