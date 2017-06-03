#pragma once

#include <opcua.h>

#include "helpers.h"

namespace opcua {

OPCUA_DEFINE_METHODS(StatusCode);

class StatusCode {
 public:
  StatusCode() { Initialize(code_); }
  StatusCode(OpcUa_StatusCode code) : code_{code} {}
  ~StatusCode() { Clear(); }

  OpcUa_StatusCode code() const { return code_; }

  void Clear() { opcua::Clear(code_); }

  bool IsGood() const { return OpcUa_IsGood(code_); }
  bool IsNotGood() const { return OpcUa_IsNotGood(code_); }
  bool IsUncertain() const { return OpcUa_IsUncertain(code_); }
  bool IsNotUncertain() const { return OpcUa_IsNotUncertain(code_); }
  bool IsBad() const { return OpcUa_IsBad(code_); }
  bool IsNotBad() const { return OpcUa_IsNotBad(code_); }

  explicit operator bool() const { return IsNotBad(); }

  bool operator==(StatusCode other) const { return code_ == other.code_; }
  bool operator!=(StatusCode other) const { return !operator==(other); }

 private:
  OpcUa_StatusCode code_;
};

inline void Check(StatusCode status_code) {
  if (status_code.IsBad())
    throw status_code;
}

inline bool operator==(StatusCode a, OpcUa_StatusCode b) {
  return a.code() == b;
}

inline bool operator==(OpcUa_StatusCode a, StatusCode b) {
  return b == a;
}

} // namespace opcua