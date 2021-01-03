#pragma once

#include <opcua.h>
#include <opcuapp/helpers.h>
#include <stdexcept>

namespace opcua {

OPCUA_DEFINE_METHODS(StatusCode);

class StatusCode {
 public:
  StatusCode() { Initialize(code_); }
  StatusCode(OpcUa_StatusCode code) : code_{code} {}
  ~StatusCode() { Clear(); }

  void Clear() { opcua::Clear(code_); }

  bool IsGood() const { return OpcUa_IsGood(code_); }
  bool IsNotGood() const { return OpcUa_IsNotGood(code_); }
  bool IsUncertain() const { return OpcUa_IsUncertain(code_); }
  bool IsNotUncertain() const { return OpcUa_IsNotUncertain(code_); }
  bool IsBad() const { return OpcUa_IsBad(code_); }
  bool IsNotBad() const { return OpcUa_IsNotBad(code_); }

  explicit operator bool() const { return IsNotBad(); }

  OpcUa_StatusCode code() const { return code_; }

 private:
  OpcUa_StatusCode code_;
};

class StatusCodeException : public std::exception {
 public:
  explicit StatusCodeException(StatusCode status_code)
      : status_code_{status_code} {}

  StatusCode status_code() const { return status_code_; }

 private:
  const StatusCode status_code_;
};

inline void Check(StatusCode status_code) {
  if (status_code.IsBad())
    throw StatusCodeException{status_code};
}

}  // namespace opcua
