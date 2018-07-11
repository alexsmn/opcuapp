#pragma once

#include <opcuapp/basic_types.h>

namespace opcua {

class DateTime {
 public:
  DateTime() { ::OpcUa_DateTime_Initialize(&value_); }

  OpcUa_DateTime get() const { return value_; }
  UInt16 picoseconds() const { return picoseconds_; }

  static DateTime UtcNow() { return DateTime{::OpcUa_DateTime_UtcNow()}; }

 private:
  explicit DateTime(OpcUa_DateTime value) : value_{value} {}

  OpcUa_DateTime value_;
  UInt16 picoseconds_ = 0;
};

}  // namespace opcua
