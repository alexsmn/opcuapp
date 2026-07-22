#include "opcua/ua/ua_service_header.h"

#include "opcua/ua/ua_binary_codec.h"

#include "opcua/transport/binary/codec_utils.h"

#include <gtest/gtest.h>

namespace opcua::ua {
namespace {

using binary::Decoder;
using binary::Encoder;

// The bytes the hand-written AppendRequestHeader (service_codec.cpp) writes.
// Reproduced here rather than #included because that function is in an
// anonymous namespace; this is the contract the migrated path must match, so
// the two encoders producing identical bytes is exactly what these goldens
// assert. If AppendRequestHeader ever changes, this test is where it must be
// mirrored.
std::vector<char> HandWrittenRequestHeader(const NodeId& authentication_token,
                                           UInt32 request_handle,
                                           std::string_view trace_parent) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(authentication_token);
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(std::uint32_t{0});
  encoder.Encode(std::string_view{""});
  encoder.Encode(std::uint32_t{0});
  if (trace_parent.empty()) {
    encoder.Encode(NodeId{});
    encoder.Encode(std::uint8_t{0x00});
  } else {
    std::vector<char> body;
    Encoder body_encoder{body};
    body_encoder.Encode(std::int32_t{1});
    body_encoder.Encode(QualifiedName{"traceparent"});
    body_encoder.Encode(Variant{String{std::string{trace_parent}}});
    encoder.Encode(binary::EncodedExtensionObject{17537, std::move(body)});
  }
  return bytes;
}

std::vector<char> HandWrittenResponseHeader(UInt32 request_handle,
                                            Status service_result) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  encoder.Encode(std::int64_t{0});
  encoder.Encode(request_handle);
  encoder.Encode(service_result.full_code());
  encoder.Encode(std::uint8_t{0});
  encoder.Encode(std::int32_t{0});
  encoder.Encode(NodeId{});
  encoder.Encode(std::uint8_t{0x00});
  return bytes;
}

template <class T>
std::vector<char> EncodeGenerated(const T& value) {
  std::vector<char> bytes;
  Encoder encoder{bytes};
  Encode(encoder, value);
  return bytes;
}

// The whole point of the header seam: encoding a generated RequestHeader built
// from the envelope fields is byte-identical to the hand-written header. If
// this holds, a service can move to the generated codec without changing a
// single byte on the wire.
TEST(UaServiceHeaderTest,
     GeneratedRequestHeaderMatchesHandWrittenWithoutTrace) {
  const NodeId token{String{"session-token"}, 1};
  EXPECT_EQ(EncodeGenerated(MakeRequestHeader(token, 42, {})),
            HandWrittenRequestHeader(token, 42, {}));

  // The null-authentication-token case (no session yet), as discovery uses.
  EXPECT_EQ(EncodeGenerated(MakeRequestHeader(NodeId{}, 7, {})),
            HandWrittenRequestHeader(NodeId{}, 7, {}));
}

TEST(UaServiceHeaderTest, GeneratedRequestHeaderMatchesHandWrittenWithTrace) {
  const NodeId token{String{"session-token"}, 1};
  const std::string trace =
      "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
  EXPECT_EQ(EncodeGenerated(MakeRequestHeader(token, 42, trace)),
            HandWrittenRequestHeader(token, 42, trace));
}

TEST(UaServiceHeaderTest, RecoversTheTraceParentFromADecodedHeader) {
  const std::string trace =
      "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
  const RequestHeader header = MakeRequestHeader(NodeId{}, 1, trace);

  // Round-trip through the wire, then recover.
  const std::vector<char> bytes = EncodeGenerated(header);
  Decoder decoder{bytes};
  RequestHeader decoded;
  ASSERT_TRUE(Decode(decoder, decoded));
  EXPECT_EQ(GetTraceParent(decoded), trace);

  // An absent additionalHeader yields an empty traceparent.
  EXPECT_TRUE(GetTraceParent(MakeRequestHeader(NodeId{}, 1, {})).empty());
}

TEST(UaServiceHeaderTest, GeneratedResponseHeaderMatchesHandWritten) {
  EXPECT_EQ(EncodeGenerated(MakeResponseHeader(42, StatusCode::Good)),
            HandWrittenResponseHeader(42, StatusCode::Good));
  EXPECT_EQ(
      EncodeGenerated(MakeResponseHeader(9, StatusCode::Bad_NodeIdUnknown)),
      HandWrittenResponseHeader(9, StatusCode::Bad_NodeIdUnknown));
}

}  // namespace
}  // namespace opcua::ua
