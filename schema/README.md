# Vendored OPC UA schema

The OPC Foundation's machine-readable description of the OPC UA type system.
`tools/gen_ua_types.py` turns these files into the C++ types, encoding-id
constants and codecs under `opcua/ua/` at build time, so those are *derived*
artifacts — fix a wrong field or a wrong id by bumping the schema, never by
editing generated output.

| File | What it defines |
| --- | --- |
| `Opc.Ua.Types.bsd` | The binary type dictionary: every StructuredType and EnumeratedType, with field order, `LengthField` for arrays and `SwitchField` for optional fields. Includes the service messages (`ReadRequest`, `ReadResponse`, …). |
| `NodeIds.csv` | Every standard NodeId, including the `*_Encoding_DefaultBinary` and `*_Encoding_DefaultJson` ids that identify a message on the wire. |
| `StatusCode.csv` | The standard StatusCodes and their numeric values. |

`VERSION` pins the exact upstream commit these were taken from.

## Provenance and licence

Taken from <https://github.com/OPCFoundation/UA-Nodeset> (directory `Schema`),
which publishes them under the **OPC Foundation MIT License 1.00** — the
copyright header inside `Opc.Ua.Types.bsd` is part of the file and must be
kept. The same files are also served from
<https://files.opcfoundation.org/schemas/UA/1.05/>, but that listing lags the
repository and several paths 404; the repository is the authoritative source.

## Bumping to a newer release

```shell
tools/fetch_schema.sh <upstream-commit-sha>
```

Then rebuild and read the diff of the generated headers before committing: a
schema bump is a **wire-visible change**. Anything it alters in the encoding
ids or in a message's field list changes what goes on the wire, so all tiers
have to be redeployed together (see the deployment notes in the repository
root `CLAUDE.md`).
