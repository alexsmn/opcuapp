#pragma once

#include <cstdint>
#include <vector>

namespace opcua {

using Boolean = bool;
using Int8 = int8_t;
using UInt8 = uint8_t;
using Int16 = int16_t;
using UInt16 = uint16_t;
using Int32 = int32_t;
using UInt32 = uint32_t;
using Int64 = int64_t;
using UInt64 = uint64_t;
using Double = double;

using NamespaceIndex = uint16_t;
using ByteString = std::vector<char>;

}  // namespace opcua (vendored)
