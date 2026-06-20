# opcuapp

A self-contained OPC UA Binary / WebSocket stack: secure-channel + wire codec,
shared client/server session and subscription runtime, and a WebSocket
(JSON-over-WS) transport. Extracted from the Telecontrol SCADA monorepo
(`common/opcua`) with **zero dependency on the SCADA `core` repo**.

## Layout

```
opcua/
  base/        vendored generic infrastructure (Awaitable, AnyExecutor, time, ...)
  scada/       vendored OPC UA domain types + service interfaces  -> namespace opcua::scada
  metrics/     vendored trace-id helper
  common/      vendored ScadaCommon helpers (data_services_util, ...)
  net/         vendored executor adapter
  *.h / *.cpp  the OPC UA stack itself (sessions, runtime, endpoints) -> namespace opcua
  binary/      OPC UA Binary wire codec, secure channel, crypto
  websocket/   OPC UA over WebSocket (Boost.Beast + JSON codec)
  test/        test fixtures
```

### Namespacing

So that a consumer can link both this library and the SCADA `core` library in
one translation unit without ODR clashes, every vendored symbol is distinct
from core's:

- OPC UA domain types / service interfaces: `scada::X` -> `opcua::scada::X`
- generic base classes: `base::X` -> `opcua::base::X`
- async infrastructure aliases: global `Awaitable` / `AnyExecutor` -> `opcua::Awaitable` / `opcua::AnyExecutor`
- low-level, domain-free utilities (UtfConvert, Format, debug_util, StructWriter,
  boost_log) are kept at global scope, matching how they were referenced in core.

The native stack lives in `namespace opcua`, so it reaches the vendored
`opcua::scada` / `opcua::base` / `opcua::Awaitable` symbols via
enclosing-namespace lookup with no per-call qualification.

## Building (standalone)

Requires Boost (asio, json, log, beast, locale), OpenSSL, and the `transport`
library (the `net` repo). Example against a vcpkg toolchain:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DOPCUAPP_TRANSPORT_DIR=/path/to/net
cmake --build build --target opcuapp
```

Consumers use `find_package(opcuapp)` (via `FindOpcuapp.cmake` on
`CMAKE_MODULE_PATH`) and link `opcuapp::opcuapp`.

## Regenerating the vendored sources

The vendored tree under `opcua/` is produced mechanically from the SCADA
monorepo by the scripts in `tools/`, run in order:

1. `tools/vendor_from_scada.py` — copies the transitive `core`/`common` include
   closure plus the native `common/opcua` sources.
2. `tools/codemod_includes.py` — rewrites include paths under the `opcua/` prefix.
3. `tools/codemod_namespace.py` — applies the namespacing described above.

The scripts expect to run from a checkout where `../../core`, `../../common`
and `../../third_party/net` exist (i.e. opcuapp sitting at
`<scada>/third_party/opcuapp`). They are idempotent.
