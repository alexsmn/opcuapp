#pragma once

#include "opcua/types/extension_object.h"
#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

// Transport-agnostic ExtensionObject decoding.
namespace opcua::ua {

// Decodes `extension_object` whichever body encoding its transport produced.
//
// An ExtensionObject body is either a binary ByteString keyed by the
// DefaultBinary id or JSON keyed by the DefaultJson id (OPC UA Part 6
// §5.4.2.16 ExtensionObject,
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.16). The two
// decoders reject each other's bodies outright — `FromExtensionObject` returns
// false for *every* JSON body — so any code that can be reached from both the
// binary and the UA-JSON transport has to try both. Reading only the binary
// form fails silently: the value decodes as "absent" rather than as an error,
// which is what forced HistoryRead onto bespoke web service names before
// `history_conversion` started trying both.
template <class T>
bool FromAnyExtensionObject(const ExtensionObject& extension_object, T& value) {
  return FromExtensionObject(extension_object, value) ||
         FromJsonExtensionObject(extension_object, value);
}

}  // namespace opcua::ua
