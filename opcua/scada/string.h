#pragma once

#include <string>

namespace opcua {

// Built-in OPC UA String: a sequence of Unicode characters, here held as a
// UTF-8 std::string. OPC UA Part 3 §8.31 String,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.31
using String = std::string;

}  // namespace opcua (vendored)
