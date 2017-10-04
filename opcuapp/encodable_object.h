#pragma once

#include <opcuapp/binary_encoder.h>
#include <opcuapp/binary_decoder.h>
#include <opcuapp/stream.h>
#include <opcuapp/structs.h>

namespace opcua {

inline void CopyEncodeable(const OpcUa_EncodeableType& type, const OpcUa_Void* source, OpcUa_Void* target) {
  assert(source);
  assert(target);

  // TODO: Optimize.
  std::vector<char> data;
  data.reserve(64);

  // Serialize.
  {
    BinaryEncoder encoder;
    VectorOutputStream stream{data};
    MessageContext context;
    encoder.Open(stream.get(), context);
    encoder.WriteEncodable(type, source);
  }

  // Deserialize.
  {
    BinaryDecoder decoder;
    MemoryInputStream stream{data.data(), data.size()};
    MessageContext context;
    decoder.Open(stream.get(), context);
    decoder.ReadEncodable(type, target);
  }
}

} // namespace opcua
