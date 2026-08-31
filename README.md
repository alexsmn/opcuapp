# opcuapp

A self-contained OPC UA Binary / WebSocket stack: secure-channel + wire codec,
shared client/server session and subscription runtime, and a WebSocket
(JSON-over-WS) transport. Extracted from the Telecontrol SCADA monorepo
(~~`common/opcua`~~) with **zero dependency on the SCADA `core` repo**.

## Layout

```
opcua/
  base/        vendored generic infrastructure (Awaitable, AnyExecutor, time, ...)
  services/    OPC UA service callback and request helper types
  monitored/   OPC UA MonitoredItem subscription boundary
  metrics/     vendored trace-id helper
  net/         vendored executor adapter
  *.h / *.cpp  the OPC UA stack itself (sessions, runtime, endpoints) -> namespace opcua
  transport/   transport backends:
    binary/      OPC UA Binary wire codec, secure channel, crypto
    websocket/   OPC UA over WebSocket (Boost.Beast + JSON codec)
  test/        test fixtures
```

### Namespacing

So that a consumer can include both this library and the SCADA `core` library in
one translation unit without ODR clashes, OPC UA protocol symbols live in
`namespace opcua` and are therefore distinct from core's:

- OPC UA protocol/service types: `opcua::X`
- MonitoredItem subscription boundary: `opcua::X`
- generic base classes: `base::X` -> `opcua::base::X`
- async infrastructure aliases: global `Awaitable` / `AnyExecutor` -> `opcua::Awaitable` / `opcua::AnyExecutor`
- low-level, domain-free utilities (`UtfConvert`, `Format`, `debug_util`'s
  `operator<<`/`ToString`, `StructWriter`, `SharedValue`, ...) -> `opcua::...`

A whole-file wrap (rather than nesting only the obvious namespaces) is required
because the codebase relies on a global `ToString` / `operator<<` overload set
that type headers extend; wrapping only part of it would shadow the rest for
namespace-nested callers. `namespace std` / `boost` / `transport` blocks (hash
specializations, Boost.Log ADL helpers, external forward declarations) are
excluded from the wrap and stay at their original scope. `boost_log.h` is left
global (its macros reference `::`-qualified names and it is only included by
.cpp internals, never a public header).

The native stack already lives in `namespace opcua`, so it reaches the
MonitoredItem subscription boundary plus `opcua::base` / `opcua::Awaitable`
symbols via enclosing-namespace lookup with no per-call qualification.

## Building (standalone)

Requires Boost (asio, json, log, beast, locale), OpenSSL, and the `transport`
library, which the `net` product provides. `net` must be checked out beside this
repository as `../net`; the resolver in `build-support/` finds it there (and in
the SCADA monorepo, where it lives at `third_party/net`).

```sh
cmake --preset ninja
cmake --build --preset release      # or: debug, relwithdebinfo
ctest --preset test-release         # or: test-debug
```

Every product in the SCADA tree carries this same preset set (ADR 0011). Set
`VCPKG_ROOT` in the environment; anything else machine-specific goes in
`.scada-local.cmake` beside `build-support/`. Output lands in
`build/ninja/bin/<config>/`.

The `ninja` preset turns `OPCUAPP_BUILD_TESTS` on. The option itself defaults
to OFF so a consumer does not pay for the 48 unit-test files, but a build of
opcuapp itself runs them.

If `net` is somewhere else, name it: `-DSCADA_PRODUCT_ROOT_NET=/path/to/net`
(or keep using `-DOPCUAPP_TRANSPORT_DIR=/path/to/net`, which still works).

Consumers use `find_package(opcuapp)` (via `Findopcuapp.cmake` on
`CMAKE_MODULE_PATH`) and link `opcuapp::opcuapp`. The module filename is
lowercase to match the call site; on a case-sensitive filesystem any other
spelling fails to resolve.

## History

The tree under `opcua/` was originally vendored mechanically from the SCADA
monorepo (`core`/`common`) by one-shot scripts in a `tools/` directory. The
sources have since diverged from the monorepo (renames, reorganization, new
features), so the vendoring scripts were removed; this repo is now the sole
home of these sources.

## License

MIT — see [LICENSE](LICENSE).
