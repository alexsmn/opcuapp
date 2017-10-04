#pragma once

#include <istream>
#include <opcua_memorystream.h>
#include <opcuapp/span.h>
#include <vector>

namespace opcua {

class StdInputStream {
 public:
  explicit StdInputStream(std::istream& stream) {
    ua_stream_.Type        = OpcUa_StreamType_Input;
    ua_stream_.Handle      = &stream;
    ua_stream_.GetPosition = &StdInputStream::GetPosition;
    ua_stream_.SetPosition = &StdInputStream::SetPosition;
    ua_stream_.Read        = &StdInputStream::Read;
  }

  OpcUa_InputStream& get() { return ua_stream_; }
  const OpcUa_InputStream& get() const { return ua_stream_; }

 private:
  static OpcUa_StatusCode GetPosition(OpcUa_Stream* strm, OpcUa_UInt32* position) {
    auto& stream = *reinterpret_cast<std::istream*>(strm->Handle);
    *position = static_cast<OpcUa_UInt32>(stream.tellg());
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

  OpcUa_InputStream ua_stream_ = {};
};

class MemoryInputStream {
 public:
  MemoryInputStream(const void* data, size_t size)
      : data_{reinterpret_cast<const char*>(data), size}  {
    ua_stream_.Type        = OpcUa_StreamType_Input;
    ua_stream_.Handle      = this;
    ua_stream_.GetPosition = &MemoryInputStream::GetPosition;
    ua_stream_.SetPosition = &MemoryInputStream::SetPosition;
    ua_stream_.Read        = &MemoryInputStream::Read;
  }

  OpcUa_InputStream& get() { return ua_stream_; }
  const OpcUa_InputStream& get() const { return ua_stream_; }
  
 private:
  static OpcUa_StatusCode GetPosition(OpcUa_Stream* strm, OpcUa_UInt32* position) {
    auto& stream = *reinterpret_cast<MemoryInputStream*>(strm->Handle);
    *position = static_cast<OpcUa_UInt32>(stream.pos_);
    return OpcUa_Good;
  }

  static OpcUa_StatusCode SetPosition(OpcUa_Stream* strm, OpcUa_UInt32 position) {
    auto& stream = *reinterpret_cast<MemoryInputStream*>(strm->Handle);
    if (position > stream.data_.size())
      return OpcUa_Bad;
    stream.pos_ = position;
    return OpcUa_Good;
  }

  static OpcUa_StatusCode Read(OpcUa_InputStream* istrm, OpcUa_Byte* buffer, OpcUa_UInt32* count) {
    auto& stream = *reinterpret_cast<MemoryInputStream*>(istrm->Handle);
    if (*count > stream.data_.size() - stream.pos_)
      return OpcUa_Bad;
    memcpy(buffer, stream.data_.data() + stream.pos_, *count);
    stream.pos_ += *count;
    return OpcUa_Good;
  }

  OpcUa_InputStream ua_stream_ = {};
  Span<const char> data_;
  size_t pos_ = 0;
};

class VectorOutputStream {
 public:
  explicit VectorOutputStream(std::vector<char>& data)
      : data_{data} {
    ua_stream_.Type        = OpcUa_StreamType_Output;
    ua_stream_.Handle      = this;
    ua_stream_.GetPosition = &VectorOutputStream::GetPosition;
    ua_stream_.SetPosition = &VectorOutputStream::SetPosition;
    ua_stream_.Write       = &VectorOutputStream::Write;
  }

  OpcUa_OutputStream& get() { return ua_stream_; }
  const OpcUa_OutputStream& get() const { return ua_stream_; }

 private:
  static OpcUa_StatusCode GetPosition(OpcUa_Stream* strm, OpcUa_UInt32* position) {
    auto& stream = *reinterpret_cast<VectorOutputStream*>(strm->Handle);
    *position = stream.pos_;
    return OpcUa_Good;
  }

  static OpcUa_StatusCode SetPosition(OpcUa_Stream* strm, OpcUa_UInt32 position) {
    auto& stream = *reinterpret_cast<VectorOutputStream*>(strm->Handle);
    stream.pos_ = position;
    return OpcUa_Good;
  }

  static OpcUa_StatusCode Write(OpcUa_OutputStream* ostrm, OpcUa_Byte* buffer, OpcUa_UInt32 count) {
    auto& stream = *reinterpret_cast<VectorOutputStream*>(ostrm->Handle);
    if (stream.pos_ + count > stream.data_.size())
      stream.data_.resize(stream.pos_ + count);
    memcpy(&stream.data_[stream.pos_], buffer, count);
    stream.pos_ += count;
    return OpcUa_Good;
  }

  OpcUa_Stream ua_stream_ = {};
  std::vector<char>& data_;
  size_t pos_ = 0;
};

class NullOutputStream {
 public:
  NullOutputStream() {
    ua_stream_.Type        = OpcUa_StreamType_Output;
    ua_stream_.Handle      = this;
    ua_stream_.GetPosition = &NullOutputStream::GetPosition;
    ua_stream_.SetPosition = &NullOutputStream::SetPosition;
    ua_stream_.Write       = &NullOutputStream::Write;
  }

  OpcUa_OutputStream& get() { return ua_stream_; }
  const OpcUa_OutputStream& get() const { return ua_stream_; }

 private:
  static OpcUa_StatusCode GetPosition(OpcUa_Stream* strm, OpcUa_UInt32* position) {
    auto& stream = *reinterpret_cast<NullOutputStream*>(strm->Handle);
    *position = stream.pos_;
    return OpcUa_Good;
  }

  static OpcUa_StatusCode SetPosition(OpcUa_Stream* strm, OpcUa_UInt32 position) {
    auto& stream = *reinterpret_cast<NullOutputStream*>(strm->Handle);
    stream.pos_ = position;
    return OpcUa_Good;
  }

  static OpcUa_StatusCode Write(OpcUa_OutputStream* ostrm, OpcUa_Byte* buffer, OpcUa_UInt32 count) {
    return OpcUa_Good;
  }

  OpcUa_OutputStream ua_stream_ = {};
  size_t pos_ = 0;
};

} // namespace opcua