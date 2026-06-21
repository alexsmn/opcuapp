#pragma once

#include <cstdint>
#include <vector>

namespace opcua {

// Built-in OPC UA Boolean: a value of true or false. OPC UA Part 3 §8.8 Boolean,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.8
using Boolean = bool;
// Built-in OPC UA SByte: a signed 8-bit integer. OPC UA Part 3 §8.17 SByte,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.17
using Int8 = int8_t;
// Built-in OPC UA Byte: an unsigned 8-bit integer. OPC UA Part 3 §8.9 Byte,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.9
using UInt8 = uint8_t;
// Built-in OPC UA Int16: a signed 16-bit integer. OPC UA Part 3 §8.25 Int16,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.25
using Int16 = int16_t;
// Built-in OPC UA UInt16: an unsigned 16-bit integer. OPC UA Part 3 §8.34
// UInt16, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.34
using UInt16 = uint16_t;
// Built-in OPC UA Int32: a signed 32-bit integer. OPC UA Part 3 §8.26 Int32,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.26
using Int32 = int32_t;
// Built-in OPC UA UInt32: an unsigned 32-bit integer. OPC UA Part 3 §8.35
// UInt32, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.35
using UInt32 = uint32_t;
// Built-in OPC UA Int64: a signed 64-bit integer. OPC UA Part 3 §8.27 Int64,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.27
using Int64 = int64_t;
// Built-in OPC UA UInt64: an unsigned 64-bit integer. OPC UA Part 3 §8.36
// UInt64, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.36
using UInt64 = uint64_t;
// Built-in OPC UA Double: an IEEE-754 double-precision float. OPC UA Part 3
// §8.12 Double, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.12
using Double = double;

// Index into a Server's NamespaceArray identifying the namespace of a NodeId or
// QualifiedName, relative to the NodeId namespace concept. OPC UA Part 3 §8.2
// NodeId, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.2
using NamespaceIndex = uint16_t;
// Built-in OPC UA ByteString: a sequence of Byte values. OPC UA Part 3 §8.10
// ByteString, https://reference.opcfoundation.org/Core/Part3/v105/docs/8.10
using ByteString = std::vector<char>;

}  // namespace opcua (vendored)
