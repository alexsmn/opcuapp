#include "opcua/events/event.h"

#include "opcua/base/debug_util.h"
#include "opcua/base/struct_writer.h"

namespace opcua {

ByteString EncodeEventIdByteString(EventId event_id) {
  ByteString bytes(8);
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>((event_id >> (8 * (7 - i))) & 0xff);
  }
  return bytes;
}

std::optional<EventId> DecodeEventIdByteString(const ByteString& bytes) {
  if (bytes.size() != 8) {
    return std::nullopt;
  }
  EventId event_id = 0;
  for (int i = 0; i < 8; ++i) {
    event_id = (event_id << 8) | static_cast<unsigned char>(bytes[i]);
  }
  return event_id;
}

std::ostream& operator<<(std::ostream& stream, const Event& event) {
  StructWriter{stream}
      .AddField("event_type_id", event.event_type_id)
      .AddField("event_id", event.event_id)
      .AddField("time", event.time)
      .AddField("receive_time", event.receive_time)
      .AddField("source_node_id", event.source_node_id)
      .AddField("source_name", event.source_name)
      .AddField("user_id", event.user_id)
      .AddField("value", event.value)
      .AddField("message", event.message)
      .AddField("acked", event.acked)
      .AddField("acknowledged_user_id", event.acknowledged_user_id)
      .AddField("acknowledged_time", event.acknowledged_time);
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const ModelChangeEvent& e) {
  constexpr std::string_view kVerbBitStrings[] = {
      "NodeAdded",        "NodeDeleted",     "ReferenceAdded",
      "ReferenceDeleted", "DataTypeChanged",
  };

  StructWriter{stream}
      .AddField("node_id", e.node_id)
      .AddField("type_definition_id", e.type_definition_id)
      .AddBitMaskField("verb", e.verb, kVerbBitStrings);
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const SemanticChangeEvent& e) {
  StructWriter{stream}.AddField("node_id", e.node_id);
  return stream;
}

}  // namespace opcua
