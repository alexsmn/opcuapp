#pragma once

#include <opcuapp/types.h>
#include <opcuapp/variant.h>

namespace opcua {

OPCUA_DEFINE_METHODS(DataValue);

inline void Copy(const OpcUa_DataValue& source, OpcUa_DataValue& target) {
  Copy(source.Value, target.Value);
  target.StatusCode = source.StatusCode;
  target.SourceTimestamp = source.SourceTimestamp;
  target.ServerTimestamp = source.ServerTimestamp;
  target.SourcePicoseconds = source.SourcePicoseconds;
  target.ServerPicoseconds = source.ServerPicoseconds;
}

class DataValue {
 public:
  DataValue() { Initialize(value_); }
  ~DataValue() { Clear(); }

  DataValue(const DataValue& source) {
    Copy(source.value_, value_);
  }

  DataValue(DataValue&& source)
      : value_{source.value_} {
    Initialize(source.value_);
  }

  DataValue& operator=(const DataValue& source) = delete;

  DataValue(StatusCode status_code) {
    Initialize(value_);
    value_.StatusCode = status_code.code();
  }

  template<class T>
  DataValue(StatusCode status_code, T&& value, const DateTime& source_timestamp, const DateTime& server_timestamp) {
    Initialize(value_);
    Variant{std::forward<T>(value)}.release(value_.Value);
    value_.StatusCode = status_code.code();
    value_.SourceTimestamp = source_timestamp.get();
    value_.ServerTimestamp = server_timestamp.get();
    value_.SourcePicoseconds = source_timestamp.picoseconds();
    value_.ServerPicoseconds = server_timestamp.picoseconds();
  }

  void Clear() { opcua::Clear(value_); }

  OpcUa_DataValue& get() { return value_; }
  const OpcUa_DataValue& get() const { return value_; }

  void release(OpcUa_DataValue& value) {
    opcua::Clear(value);
    value_ = value;
    opcua::Initialize(value);
  }

 private:
  OpcUa_DataValue value_;
};

} // namespace opcua
