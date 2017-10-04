#pragma once

#include <opcuapp/binary_encoder.h>
#include <opcuapp/binary_decoder.h>
#include <opcuapp/stream.h>
#include <opcuapp/structs.h>

namespace opcua {

inline void CopyEncodeable(const OpcUa_EncodeableType& type, const OpcUa_Void* source, OpcUa_Void* target) {
  assert(source);
  assert(target);

  MessageContext context;

  // Estimate required memory size.
  size_t size = 0;
  {
    BinaryEncoder encoder;
    NullOutputStream null_stream;
    encoder.Open(null_stream.get(), context);
    encoder.WriteEncodable(type, source, nullptr, &size);
  }

  // TODO: Optimize.
  std::vector<char> data(size);

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

} // namespace opcua
