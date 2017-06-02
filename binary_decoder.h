#pragma once

#include <istream>
#include <map>
#include <opcua_binaryencoder.h>
#include <opcua_memorystream.h>

#include "opcua/status_code.h"
#include "opcua/structs.h"
#include "opcua/types.h"
#include "opcua/node_id.h"
#include "opcua/expanded_node_id.h"

namespace opcua {

class StdStream {
 public:
  explicit StdStream(std::istream& stream) {
    ua_stream_.Type        = OpcUa_StreamType_Input;
    ua_stream_.Handle      = &stream;
    ua_stream_.GetPosition = &StdStream::GetPosition;
    ua_stream_.SetPosition = &StdStream::SetPosition;
    ua_stream_.Read        = &StdStream::Read;
  }

  ~StdStream() {}

  OpcUa_Stream& get() { return ua_stream_; }

 private:
  static OpcUa_StatusCode GetPosition(OpcUa_Stream* strm, OpcUa_UInt32* position) {
    auto& stream = *reinterpret_cast<std::istream*>(strm->Handle);
    *position = stream.tellg();
    return OpcUa_Good;
  }

  static OpcUa_StatusCode SetPosition(OpcUa_Stream* strm, OpcUa_UInt32 position) {
    auto& stream = *reinterpret_cast<std::istream*>(strm->Handle);
    stream.seekg(position);
    return OpcUa_Good;
  }

  static OpcUa_StatusCode Read(OpcUa_InputStream* istrm, OpcUa_Byte* buffer, OpcUa_UInt32* count) {
    auto& stream = *reinterpret_cast<std::istream*>(istrm->Handle);
    assert(stream);
    return stream.read(reinterpret_cast<char*>(buffer), *count) ? OpcUa_Good : OpcUa_Bad;
  }

  OpcUa_Stream ua_stream_ = {};
};

class MemoryStream {
 public:
  MemoryStream(const void* data, size_t size) {
    Check(::OpcUa_MemoryStream_CreateReadable(reinterpret_cast<OpcUa_Byte*>(const_cast<void*>(data)), size, &stream_));
  }

  ~MemoryStream() {
    if (stream_)
      stream_->Delete(&stream_);
  }

  OpcUa_InputStream* get() { return stream_; }
  
 private:
  OpcUa_InputStream* stream_ = OpcUa_Null;
};

#define BINARY_DECODER_READ(DecodeType) \
  template<> DecodeType Read(OpcUa_StringA field_name) { \
    OpcUa_##DecodeType value; \
    Check(decoder_->Read##DecodeType(reinterpret_cast<OpcUa_Decoder*>(decode_context_), field_name, &value)); \
    return value; \
  }

class BinaryDecoder {
 public:
  BinaryDecoder() {
    Check(::OpcUa_BinaryDecoder_Create(&decoder_));
  }

  BinaryDecoder(const BinaryDecoder&) = delete;

  BinaryDecoder(BinaryDecoder&& source) : decoder_{source.decoder_} {
    source.decoder_ = nullptr;
  }

  ~BinaryDecoder() {
    if (decode_context_)
      decoder_->Close(decoder_, &decode_context_);
    ::OpcUa_Decoder_Delete(&decoder_);
  }

  BinaryDecoder& operator=(const BinaryDecoder&) = delete;

  BinaryDecoder& operator=(BinaryDecoder&& source) {
    if (this != &source) {
      ::OpcUa_Decoder_Delete(&decoder_);
      decoder_ = source.decoder_;
      source.decoder_ = nullptr;
    }
    return *this;
  }

  void Open(OpcUa_InputStream& stream, OpcUa_MessageContext& context) {
    Check(decoder_->Open(decoder_, &stream, &context, &decode_context_));
  }

  void Close() {
    Check(decoder_->Close(decoder_, &decode_context_));
  }

  using NamespaceMapping = std::map<NamespaceIndex, NamespaceIndex>;

  void set_namespace_mapping(NamespaceMapping mapping) {
    namespace_mapping_ = std::move(mapping);
  }

  template<typename T> T Read(OpcUa_StringA field_name = nullptr);

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
  BINARY_DECODER_READ(ExpandedNodeId);
  BINARY_DECODER_READ(Variant);
  BINARY_DECODER_READ(QualifiedName);
  BINARY_DECODER_READ(LocalizedText);
//  BINARY_DECODER_READ(ExtensionObject);

  template<> NodeId Read(OpcUa_StringA field_name) {
    OpcUa_NodeId value;
    Check(decoder_->ReadNodeId(reinterpret_cast<OpcUa_Decoder*>(decode_context_), field_name, &value));
    value.NamespaceIndex = MapNamespaceIndex(value.NamespaceIndex);
    return value;
  }

  template<typename T>
  T ReadEnum(OpcUa_StringA field_name = nullptr) {
    return static_cast<T>(Read<Int32>(field_name));
  }

  template<typename T>
  std::vector<T> ReadArray() {
    auto len = Read<Int32>();
    if (len == -1)
      return {};
    std::vector<T> array(static_cast<size_t>(len));
    for (auto& v : array)
      v = Read<T>();
    return array;
  }

 private:
  NamespaceIndex MapNamespaceIndex(NamespaceIndex namespace_index) const {
    auto i = namespace_mapping_.find(namespace_index);
    return i == namespace_mapping_.end() ? namespace_index : i->second;
  }

  OpcUa_Decoder* decoder_ = OpcUa_Null;
  OpcUa_Handle decode_context_ = OpcUa_Null;
  NamespaceMapping namespace_mapping_;
};

} // namespace opcua