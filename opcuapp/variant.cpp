#include <opcuapp/variant.h>

#include <opcuapp/binary_encoder.h>
#include <opcuapp/binary_decoder.h>
#include <opcuapp/extension_object.h>
#include <opcuapp/stream.h>
#include <opcuapp/structs.h>

namespace opcua {

void DeepCopy(const OpcUa_Variant& source, OpcUa_Variant& target) {
  MessageContext context;

  // Estimate required memory size.
  size_t size = 0;
  {
    BinaryEncoder encoder;
    NullOutputStream null_stream;
    encoder.Open(null_stream.get(), context);
    encoder.Write(source, nullptr, &size);
  }

  // TODO: Optimize.
  std::vector<char> data(size);

  // Serialize.
  {
    BinaryEncoder encoder;
    VectorOutputStream stream{data};
    encoder.Open(stream.get(), context);
    encoder.Write(source);
  }

  // Deserialize.
  {
    BinaryDecoder decoder;
    MemoryInputStream stream{data.data(), size};
    decoder.Open(stream.get(), context);
    decoder.Read<Variant>().release(target);
  }
}

Variant::Variant(ExtensionObject&& extension_object) {
  Initialize(value_);
  value_.Datatype = OpcUaType_ExtensionObject;
  ::OpcUa_ExtensionObject_Create(&value_.Value.ExtensionObject);
  extension_object.Release(*value_.Value.ExtensionObject);
}

} // namespace opcua