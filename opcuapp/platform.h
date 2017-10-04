#pragma once

#include <opcuapp/status_code.h>

namespace opcua {

class Platform {
 public:
  Platform();
  explicit Platform(StatusCode& status_code);
  ~Platform();

  OpcUa_Handle handle() const { return handle_; }

 private:
  StatusCode status_code_ = OpcUa_Bad;
  OpcUa_Handle handle_ = OpcUa_Null;
};

inline Platform::Platform() {
  status_code_ = ::OpcUa_P_Initialize(&handle_);
  Check(status_code_);
}

inline Platform::Platform(StatusCode& status_code) {
  status_code = status_code_ = ::OpcUa_P_Initialize(&handle_);
}

inline Platform::~Platform() {
  if (status_code_)
    ::OpcUa_P_Clean(&handle_);
}

} // namespace opcua
