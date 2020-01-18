#pragma once

#include <opcuapp/basic_structs.h>
#include <opcuapp/binary_decoder.h>
#include <opcuapp/binary_encoder.h>
#include <opcuapp/stream.h>

namespace opcua {

inline void CopyEncodeable(const OpcUa_EncodeableType& type,
                           const OpcUa_Void* source,
                           OpcUa_Void* target) {
  assert(source);
  assert(target);

  // TODO: Optimize.
  std::vector<char> data;
  data.reserve(64);

  MessageContext context;
  context.KnownTypes = &OpcUa_ProxyStub_g_EncodeableTypes;
  context.NamespaceUris = &OpcUa_ProxyStub_g_NamespaceUris;
  context.AlwaysCheckLengths = OpcUa_False;

  // Serialize.
  {
    BinaryEncoder encoder;
    VectorOutputStream stream{data};
    encoder.Open(stream.get(), context);
    encoder.WriteEncodable(type, source);
  }

  // Deserialize.
  {
    BinaryDecoder decoder;
    MemoryInputStream stream{data.data(), data.size()};
    decoder.Open(stream.get(), context);
    decoder.ReadEncodable(type, target);
  }
}

}  // namespace opcua
