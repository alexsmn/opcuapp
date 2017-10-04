#pragma once

#include <opcuapp/basic_types.h>

inline bool operator<(const OpcUa_Guid& a, const OpcUa_Guid& b) {
  if (a.Data1 != b.Data1)
    return a.Data1 < b.Data1;
  if (a.Data2 != b.Data2)
    return a.Data2 < b.Data2;
  if (a.Data3 != b.Data3)
    return a.Data3 < b.Data3;
  return std::lexicographical_compare(
      std::begin(a.Data4), std::end(a.Data4),
      std::begin(b.Data4), std::end(b.Data4));
}
