#include "opcua/ua/ua_service_header.h"

#include "opcua/ua/ua_binary_codec.h"
#include "opcua/ua/ua_json_codec.h"

#include "opcua/transport/binary/codec_utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace opcua::ua {
namespace {

using binary::Decoder;
using binary::Encoder;

// The traceparent every test in this file uses, and the SpanContextDataType it
// must map to.
//
// The two are spelled out independently — the structure is written as literal
// field values rather than produced by TraceParentToSpanContext — because the
// Guid mapping is a *convention this project chose*, not something Part 26
// specifies, and a test that derived one from the other could not catch the
// convention changing. Read together they say: the 32 hex digits of the trace
// id are the Guid's canonical 8-4-4-4-12 text form
// (4bf92f35-77b3-4da6-a3ce-929d0e0e4736), and the 16 hex digits of the span id
// are a big-endian UInt64.
constexpr std::string_view kTraceParent =
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

SpanContextDataType ExpectedSpanContext() {
  return SpanContextDataType{
      .trace_id = Guid{0x4bf92f35,
                       0x77b3,
                       0x4da6,
                       {0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36}},
      .span_id = 0x00f067aa0ba902b7ull};
}

// The bytes the hand-written AppendRequestHeader (service_codec.cpp) writes.
// Reproduced here rather than #included because that function is in an
// anonymous namespace; this is the contract the migrated path must match, so
// the two encoders producing identical bytes is exactly what these goldens
// assert. If AppendRequestHeader ever changes, this test is where it must be
// mirrored.
//
// The traced form carries *two* parameters: the W3C "traceparent" and Part 26's
// "SpanContext" (OPC UA Part 26 §5.6.4,
// https://reference.opcfoundation.org/Core/Part26/v105/docs/5.6.4). The
// extension-free form is unchanged and must stay so — that is the guarantee
// that peers predating the slot still see byte-identical headers.
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
    body_encoder.Encode(std::int32_t{2});
    body_encoder.Encode(QualifiedName{"traceparent"});
    body_encoder.Encode(Variant{String{std::string{trace_parent}}});
    body_encoder.Encode(QualifiedName{"SpanContext"});
    body_encoder.Encode(Variant{ToExtensionObject(ExpectedSpanContext())});
    encoder.Encode(binary::EncodedExtensionObject{17537, std::move(body)});
  }
  return bytes;
}

// A header carrying only the Part 26 entry, as a conformant third-party client
// that has never heard of the W3C key would send.
RequestHeader SpanContextOnlyHeader(const SpanContextDataType& span_context) {
  RequestHeader header;
  SetAdditionalParameter(header, "SpanContext",
                         Variant{ToExtensionObject(span_context)});
  return header;
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
  EXPECT_EQ(EncodeGenerated(MakeRequestHeader(token, 42, kTraceParent)),
            HandWrittenRequestHeader(token, 42, kTraceParent));
}

TEST(UaServiceHeaderTest, RecoversTheTraceParentFromADecodedHeader) {
  const RequestHeader header = MakeRequestHeader(NodeId{}, 1, kTraceParent);

  // Round-trip through the wire, then recover.
  const std::vector<char> bytes = EncodeGenerated(header);
  Decoder decoder{bytes};
  RequestHeader decoded;
  ASSERT_TRUE(Decode(decoder, decoded));
  EXPECT_EQ(GetTraceParent(decoded), kTraceParent);

  // An absent additionalHeader yields an empty traceparent.
  EXPECT_TRUE(GetTraceParent(MakeRequestHeader(NodeId{}, 1, {})).empty());
}

// --- Part 26 trace context --------------------------------------------------

// The mapping convention itself: the literal structure in ExpectedSpanContext
// is what a traceparent must become, and what must come back out.
TEST(UaServiceHeaderTest, TraceParentRoundTripsThroughTheSpanContextForm) {
  const std::optional<SpanContextDataType> span_context =
      TraceParentToSpanContext(kTraceParent);
  ASSERT_TRUE(span_context.has_value());
  EXPECT_EQ(span_context->trace_id, ExpectedSpanContext().trace_id);
  EXPECT_EQ(span_context->span_id, ExpectedSpanContext().span_id);

  EXPECT_EQ(SpanContextToTraceParent(*span_context), kTraceParent);
}

// An unsampled traceparent survives the W3C entry unchanged; the Part 26 form
// cannot express the flag, so what comes back out of it alone is sampled.
TEST(UaServiceHeaderTest, SpanContextCannotCarryTheSampledFlag) {
  const std::string unsampled =
      "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00";
  const RequestHeader header = MakeRequestHeader(NodeId{}, 1, unsampled);
  EXPECT_EQ(GetTraceParent(header), unsampled);

  const std::optional<SpanContextDataType> span_context =
      TraceParentToSpanContext(unsampled);
  ASSERT_TRUE(span_context.has_value());
  EXPECT_EQ(SpanContextToTraceParent(*span_context), kTraceParent);
}

// A conformant Part 26 client sends only "SpanContext"; its trace must still
// continue into this server.
TEST(UaServiceHeaderTest, SpanContextOnlyHeaderYieldsASampledTraceParent) {
  EXPECT_EQ(GetTraceParent(SpanContextOnlyHeader(ExpectedSpanContext())),
            kTraceParent);
}

// The W3C entry wins: it is the only form carrying the sampled flag.
TEST(UaServiceHeaderTest, TraceParentWinsWhenBothFormsArePresent) {
  RequestHeader header = SpanContextOnlyHeader(
      SpanContextDataType{.trace_id = ExpectedSpanContext().trace_id,
                          .span_id = 0xdeadbeefdeadbeefull});
  SetAdditionalParameter(header, "traceparent",
                         Variant{String{std::string{kTraceParent}}});

  EXPECT_EQ(GetTraceParent(header), kTraceParent);
}

// A malformed W3C entry does not shadow a usable Part 26 one.
TEST(UaServiceHeaderTest, MalformedTraceParentFallsThroughToSpanContext) {
  RequestHeader header = SpanContextOnlyHeader(ExpectedSpanContext());
  SetAdditionalParameter(header, "traceparent", Variant{String{"nonsense"}});

  EXPECT_EQ(GetTraceParent(header), kTraceParent);
}

// Part 26 §5.6.2 Table 11: SpanId 0 is invalid, and a null TraceId identifies
// nothing. Neither may fail the request — they read as "no trace context".
TEST(UaServiceHeaderTest, InvalidSpanContextIdsReadAsAbsent) {
  EXPECT_TRUE(GetTraceParent(SpanContextOnlyHeader(SpanContextDataType{
                                 .trace_id = ExpectedSpanContext().trace_id,
                                 .span_id = 0}))
                  .empty());
  EXPECT_TRUE(GetTraceParent(SpanContextOnlyHeader(SpanContextDataType{
                                 .trace_id = Guid{},
                                 .span_id = ExpectedSpanContext().span_id}))
                  .empty());

  // A "SpanContext" holding something that is not a Structure at all.
  RequestHeader wrong_type;
  SetAdditionalParameter(wrong_type, "SpanContext", Variant{Int32{7}});
  EXPECT_TRUE(GetTraceParent(wrong_type).empty());

  EXPECT_FALSE(TraceParentToSpanContext("nonsense").has_value());
  EXPECT_FALSE(TraceParentToSpanContext({}).has_value());
}

// The extension slot is read on the binary transport and on UA-JSON, whose
// ExtensionObject bodies the binary decoder rejects outright. Reading only the
// binary form would make every websocket request look untraced.
//
// A request arriving over UA-JSON carries JSON bodies at *both* levels — the
// AdditionalParametersType envelope in additionalHeader, and the
// SpanContextDataType inside it — so both decoders have to be any-encoding.
// Building the header with SetAdditionalParameter would not reproduce this: it
// binary-encodes the envelope, and the binary path would then carry the test.
TEST(UaServiceHeaderTest, TraceContextDecodesFromJsonBodies) {
  AdditionalParametersType parameters;
  parameters.parameters.push_back(
      {.key = QualifiedName{"SpanContext"},
       .value = Variant{ToJsonExtensionObject(ExpectedSpanContext())}});

  RequestHeader header;
  header.additional_header = ToJsonExtensionObject(parameters);

  EXPECT_EQ(GetTraceParent(header), kTraceParent);
}

// The bytes a conformant third party would put on the wire, spelled out here
// from the spec rather than produced by this project's codec.
//
// This is the closest thing to an interop test that needs no second stack: if
// our reader accepts exactly these bytes, a Part 26 peer's header decodes, and
// the Guid convention is right in the only place it can be checked — the
// encoding itself. Layout, outermost first:
//
//   additionalHeader ExtensionObject
//     NodeId typeId       = i=17537
//     (AdditionalParametersType_Encoding_DefaultBinary) encoding            =
//     0x01 (ByteString body)      Part 6 §5.2.2.15 body                = Int32
//     count, then count KeyValuePairs
//       QualifiedName key = UInt16 ns=0, String "SpanContext"
//       Variant value     = mask 22 (ExtensionObject built-in), then
//         NodeId typeId   = i=19754
//         (SpanContextDataType_Encoding_DefaultBinary) encoding        = 0x01
//         body            = Guid TraceId, UInt64 SpanId   Part 6 §5.2.2.6
//
// The Guid's four fields are little-endian (Part 3 §8.14, Part 6 §5.2.2.6), so
// the first eight bytes appear byte-swapped relative to the trace id's text —
// which is exactly the transposition a raw-byte convention would have got
// wrong, and why this test spells them out.
TEST(UaServiceHeaderTest, DecodesSpanContextBytesWrittenFromTheSpec) {
  const std::vector<std::uint8_t> kBytes = {
      // --- additionalHeader ExtensionObject ---
      0x01, 0x00, 0x81, 0x44,  // NodeId four-byte form: ns=0, i=17537
      0x01,                    // body is a ByteString
      0x37, 0x00, 0x00, 0x00,  // ByteString length = 55: 4 + 17 + 34
      // --- AdditionalParametersType body ---
      0x01, 0x00, 0x00, 0x00,  // Int32 NoOfParameters = 1
      0x00, 0x00,              // QualifiedName namespace index = 0
      0x0b, 0x00, 0x00, 0x00,  // String length = 11
      'S', 'p', 'a', 'n', 'C', 'o', 'n', 't', 'e', 'x', 't',
      22,                      // Variant encoding mask: ExtensionObject
      0x01, 0x00, 0x2a, 0x4d,  // NodeId four-byte form: ns=0, i=19754
      0x01,                    // body is a ByteString
      0x18, 0x00, 0x00, 0x00,  // ByteString length = 24
      // Guid TraceId 4bf92f35-77b3-4da6-a3ce-929d0e0e4736
      0x35, 0x2f, 0xf9, 0x4b,                          // Data1, little endian
      0xb3, 0x77,                                      // Data2, little endian
      0xa6, 0x4d,                                      // Data3, little endian
      0xa3, 0xce, 0x92, 0x9d, 0x0e, 0x0e, 0x47, 0x36,  // Data4, verbatim
      // UInt64 SpanId 0x00f067aa0ba902b7, little endian
      0xb7, 0x02, 0xa9, 0x0b, 0xaa, 0x67, 0xf0, 0x00};

  Decoder decoder{std::span<const char>{
      reinterpret_cast<const char*>(kBytes.data()), kBytes.size()}};
  RequestHeader header;
  ASSERT_TRUE(decoder.Decode(header.additional_header));

  EXPECT_EQ(GetTraceParent(header), kTraceParent);
}

// A value that is not a traceparent has no SpanContextDataType form, so only
// the W3C key goes out — and it still round-trips, because this layer treats
// that field as opaque and always has.
TEST(UaServiceHeaderTest, AnUnparsableTraceParentTravelsUnderTheW3CKeyOnly) {
  const RequestHeader header = MakeRequestHeader(NodeId{}, 1, "legacy-uuid-id");

  const AdditionalParametersType parameters = GetAdditionalParameters(header);
  ASSERT_EQ(parameters.parameters.size(), 1u);
  EXPECT_EQ(parameters.parameters[0].key.name(), "traceparent");
  EXPECT_EQ(FindAdditionalParameter(parameters, "SpanContext"), nullptr);

  EXPECT_EQ(GetTraceParent(header), "legacy-uuid-id");
}

// --- The additionalHeader extension slot -----------------------------------

// The encode path stamps the envelope onto a header a service body has already
// populated. Overwriting it instead — which is what EncodeServiceRequest used
// to do — silently discards the body's own extensions.
TEST(UaServiceHeaderTest, RequestEnvelopePreservesExistingParameters) {
  const std::string trace{kTraceParent};

  RequestHeader header;
  SetAdditionalParameter(header, "vendor-parameter", Variant{Int32{7}});
  ApplyRequestEnvelope(header, NodeId{}, 7, trace);

  const std::vector<char> bytes = EncodeGenerated(header);
  Decoder decoder{bytes};
  RequestHeader decoded;
  ASSERT_TRUE(Decode(decoder, decoded));

  EXPECT_EQ(decoded.request_handle, 7u);
  EXPECT_EQ(GetTraceParent(decoded), trace);
  const AdditionalParametersType parameters = GetAdditionalParameters(decoded);
  const Variant* value =
      FindAdditionalParameter(parameters, "vendor-parameter");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->get<Int32>(), 7);
}

// An extension slot is optional by construction, so anything unreadable in it
// must read as "absent" rather than failing the request.
TEST(UaServiceHeaderTest, AbsentOrUnreadableParametersReadAsEmpty) {
  EXPECT_TRUE(GetAdditionalParameters(RequestHeader{}).parameters.empty());
  EXPECT_TRUE(GetAdditionalParameters(MakeRequestHeader(NodeId{}, 1, {}))
                  .parameters.empty());
  EXPECT_EQ(FindAdditionalParameter(GetAdditionalParameters(RequestHeader{}),
                                    "anything"),
            nullptr);
}

TEST(UaServiceHeaderTest, AdditionalParameterReplacesRatherThanDuplicates) {
  RequestHeader header;
  SetAdditionalParameter(header, "key", Variant{Int32{1}});
  SetAdditionalParameter(header, "other", Variant{Int32{2}});
  SetAdditionalParameter(header, "key", Variant{Int32{3}});

  const AdditionalParametersType parameters = GetAdditionalParameters(header);
  ASSERT_EQ(parameters.parameters.size(), 2u);
  EXPECT_EQ(parameters.parameters[0].key.name(), "key");
  EXPECT_EQ(FindAdditionalParameter(parameters, "key")->get<Int32>(), 3);
  EXPECT_EQ(FindAdditionalParameter(parameters, "other")->get<Int32>(), 2);
  EXPECT_EQ(FindAdditionalParameter(parameters, "absent"), nullptr);
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
