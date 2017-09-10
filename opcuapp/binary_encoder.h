#pragma once

#include <cassert>
#include <opcua_binaryencoder.h>

#include <opcuapp/status_code.h>

namespace opcua {

class BinaryEncoder {
 public:
  BinaryEncoder() {
    Check(::OpcUa_BinaryEncoder_Create(&encoder_));
  }

  ~BinaryEncoder() {
    if (encode_context_)
      encoder_->Close(encoder_, &encode_context_);
    ::OpcUa_Encoder_Delete(&encoder_);
  }

  BinaryEncoder(const BinaryEncoder&) = delete;
  BinaryEncoder& operator=(const BinaryEncoder&) = delete;

  void Open(OpcUa_InputStream& stream, OpcUa_MessageContext& context) {
    Check(encoder_->Open(encoder_, &stream, &context, &encode_context_));
  }

  void Close() {
    Check(encoder_->Close(encoder_, &encode_context_));
  }

  void Write(const OpcUa_Variant& source, const char* field_name = nullptr, size_t* size = nullptr) const {
    assert(encode_context_);
    OpcUa_Int32 int_size = 0;
    Check(encoder_->WriteVariant(reinterpret_cast<OpcUa_Encoder*>(encode_context_),
        const_cast<OpcUa_StringA>(field_name), &const_cast<OpcUa_Variant&>(source), size ? &int_size : nullptr));
    if (size)
      *size = static_cast<size_t>(int_size);
  }

  void WriteEncodable(const OpcUa_EncodeableType& type, const OpcUa_Void* object,
      const char* field_name = nullptr, size_t* size = nullptr) {
    assert(encode_context_);
    OpcUa_Int32 int_size = 0;
    Check(encoder_->WriteEncodeable(reinterpret_cast<OpcUa_Encoder*>(encode_context_),
        const_cast<OpcUa_StringA>(field_name), const_cast<OpcUa_Void*>(object),
        &const_cast<OpcUa_EncodeableType&>(type), size ? &int_size : nullptr));
    if (size)
      *size = static_cast<size_t>(int_size);
  }

 private:
  OpcUa_Encoder* encoder_ = OpcUa_Null;
  OpcUa_Handle encode_context_ = OpcUa_Null;
};

} // namespace opcua