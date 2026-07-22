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
