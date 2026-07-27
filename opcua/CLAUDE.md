# OPC UA

This directory contains the in-repo OPC UA Binary adapter and shared OPC UA server-runtime code.

## The type system is generated, not transcribed

The OPC Foundation's machine-readable schema is **vendored** at `../schema/`
(pinned to an upstream commit in `../schema/VERSION`), and
`../tools/gen_ua_types.py` turns it into `opcua/ua/*.h` at build time:

- `ua_types.h` — every StructuredType and EnumeratedType (357 structs, 68
  enums), members in snake_case.
- `ua_encoding_ids.h` — the `*_Encoding_DefaultBinary` / `DefaultJson` ids that
  identify a message on the wire.
- `ua_status_codes.h` — the standard StatusCodes as full 32-bit values.
- `ua_binary_codec.{h,cpp}` — `Encode`/`Decode` for every generated type, plus
  `BinaryEncodingId<T>` and `To/FromExtensionObject`.
- `ua_json_codec.{h,cpp}` — the conformant OPC UA JSON encoding (Part 6 §5.4),
  in the compact form the published service schema describes. The built-in
  layer it bottoms out in is hand-written in `ua/ua_json_builtins.{h,cpp}`,
  mirroring how the binary codec bottoms out in `codec_utils.cpp`. Also
  `JsonEncodingId<T>` and `To/FromJsonExtensionObject`.

**The two ExtensionObject helper pairs are not interchangeable.** An
ExtensionObject body is either a binary ByteString keyed by the DefaultBinary
id, or JSON keyed by the DefaultJson id (Part 6 §5.4.2.16).
`ua::FromExtensionObject` requires `binary_body()` and returns false for
*every* JSON body — so on the UA-JSON transport it silently reports a type
mismatch rather than decoding. Use `FromJsonExtensionObject` there.

This is what forced the history services onto bespoke web message shapes for so
long: HistoryRead was split into `HistoryReadRaw` / `HistoryReadEvents` and
HistoryUpdate carried a hand-rolled `Details` envelope, because their details
could not cross a JSON transport at all. Both now travel as their conformant
services. `history_conversion` is transport-agnostic and so tries **both**
decoders (`FromAnyExtensionObject`); the websocket codec transcodes bodies to
JSON on the way out (`WithJsonBodies`), since a JSON-only peer — the web
client — has no decoder for a base64 binary body.

The generator cross-checks the binary dictionary against the vendored JSON
schema (`schema/opc.ua.services.jsonschema.json`): after resolving the JSON
schema's `allOf` inheritance, every shared structure's field list must match,
or the build fails. Two OPC Foundation artifacts agreeing is what lets the JSON
codec reuse the dictionary's field names.

Do not hand-write a message struct or an encoding-id constant. If a field or an
id is wrong, the schema is the thing to bump (`../tools/fetch_schema.sh <sha>`);
see `../schema/README.md`.

The **built-in** types are the exception and stay hand-written under
`types/`: NodeId, ExpandedNodeId, Variant, DataValue, DiagnosticInfo,
LocalizedText, QualifiedName, ExtensionObject, Guid, XmlElement. They have
bit-packed encodings and hand-tuned storage, they never change, and the
generator explicitly skips them (`BUILT_IN_STRUCTS`). Their codec lives in
`transport/binary/codec_utils.cpp`, driven by the single
`OPCUA_VARIANT_BUILT_IN_TYPES` list — add a built-in there and every encode and
decode path picks it up at once.

`opcua/ua/ua_types.cpp` exists to compile the generated headers into the
library and to pin, with `static_assert`, the facts the hand-written codec
depends on (encoding ids, status-code values, key field types).

Remember that a schema bump is **wire-visible**: old and new builds may not
interoperate, so all tiers deploy together.
