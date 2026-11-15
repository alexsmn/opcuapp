# OPC UA

This directory contains the in-repo OPC UA Binary adapter and shared OPC UA server-runtime code.

The long form — the reasoning and history behind each rule, under the same headings — is in [`CLAUDE.reference.md`](CLAUDE.reference.md).

## The type system is generated, not transcribed

The OPC Foundation's machine-readable schema is **vendored** at `../schema/`
(pinned to an upstream commit in `../schema/VERSION`), and
`../tools/gen_ua_types.py` turns it into `opcua/ua/*.h` at build time:

- `ua_types.h` — every StructuredType and EnumeratedType, members in snake_case.
- `ua_encoding_ids.h` — the `*_Encoding_DefaultBinary` / `DefaultJson` ids that
  identify a message on the wire.
- `ua_status_codes.h` — the standard StatusCodes as full 32-bit values.
- `ua_binary_codec.{h,cpp}` — `Encode`/`Decode` for every generated type, plus
  `BinaryEncodingId<T>` and `To/FromExtensionObject`.
- `ua_json_codec.{h,cpp}` — the conformant OPC UA JSON encoding ([Part 6 §5.4
  "OPC UA JSON"](https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4)),
  compact form. Its built-in layer is hand-written in
  `ua/ua_json_builtins.{h,cpp}`. Also `JsonEncodingId<T>` and
  `To/FromJsonExtensionObject`.

**The two ExtensionObject helper pairs are not interchangeable.** A body is
either binary keyed by the DefaultBinary id, or JSON keyed by the DefaultJson id
([Part 6 §5.4.2.16
"ExtensionObject"](https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.16)).
`ua::FromExtensionObject` returns false for *every* JSON body, so on the UA-JSON
transport use `FromJsonExtensionObject`. Code that may see either transport
tries both (`FromAnyExtensionObject`, as `history_conversion` does); the
websocket codec transcodes bodies to JSON on the way out (`WithJsonBodies`)
because a JSON-only peer cannot decode a binary body.

The generator cross-checks the binary dictionary against the vendored JSON
schema (`schema/opc.ua.services.jsonschema.json`); a field-list mismatch fails
the build.

Do not hand-write a message struct or an encoding-id constant. If a field or an
id is wrong, bump the schema (`../tools/fetch_schema.sh <sha>`); see
`../schema/README.md`.

**The SecureChannel handshake is not an exception, but its framing is.** The
bodies (`OpenSecureChannelRequest`/`Response`, `CloseSecureChannelRequest`) are
generated types aliased into `opcua::binary` by `transport/binary/secure_channel.h`
and encoded by `ua::Encode`/`ua::Decode` on their own `kBinaryEncodingId`. Only
the `SecureConversationMessage` and its security and sequence headers stay
hand-written: they are framing from [Part 6 §6.7 "OPC UA Secure
Conversation"](https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7),
not StructuredTypes.

`ua_encoding_ids.h` defines names like `kOpenSecureChannelRequest` **twice**
(`binary_encoding_id` and `json_encoding_id`). Prefer `T::kBinaryEncodingId`: it
cannot be paired with the wrong codec.

The **built-in** types stay hand-written under `types/`: NodeId,
ExpandedNodeId, Variant, DataValue, DiagnosticInfo, LocalizedText,
QualifiedName, ExtensionObject, Guid, XmlElement (the generator skips them via
`BUILT_IN_STRUCTS`). Their codec lives in `transport/binary/codec_utils.cpp`,
driven by the single `OPCUA_VARIANT_BUILT_IN_TYPES` list — add a built-in there
and every encode and decode path picks it up.

`opcua/ua/ua_types.cpp` compiles the generated headers and pins, with
`static_assert`, the facts the hand-written codec depends on.

A schema bump is **wire-visible**: old and new builds may not interoperate, so
all tiers deploy together.

## Client requests are bounded; Publish is the exception

`ClientChannel::Call` applies a deadline (`kDefaultClientRequestTimeout`, 30 s,
overridable via `Context::request_timeout`) and answers `Bad_Timeout`.
**`Receive` does not, and must not by default** — Publish uses it, and [Part 4
§5.14.5 Publish](https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5)
has the server hold a Publish request until data is available. A caller that
wants a bounded `Receive` passes one explicitly.

Every wait must be bounded at this layer, because against a half-open
connection the transport never raises an error and nothing else completes a
pending entry.

**Do not "fix" a hang like that one layer up.** A caller-side timeout only moves
the wedge: the recovery path (`ClientSession::Disconnect` →
`ClientProtocolSession::Close`) sends CloseSession on the same dead connection.

The bounds that exist, and must stay:

- **Writes:** `kDefaultWriteTimeout` (30 s) on `ClientTransport::WriteFrame`.
  The handler resets the *transport*, failing the parked write; the failed send
  releases the send turn, so later requests do not park in `WaitForSendTurn`.
- **A write must never be abandoned in place.** A half-written frame
  desynchronises the stream; tearing the transport down is the only safe answer,
  which is why the deadline ends the connection. Pinned by
  `ClientTransportConnectTimeoutTest.WriteTimesOutWhenThePeerStopsDraining`.
- **A torn-down transport is refused, not touched.** `WriteFrame` and
  `ReadFrameWithin` return `Bad_NoCommunication` before touching the executor (a
  reset `any_transport` has an empty executor, and building a timer on it
  throws). Pinned by `WriteAfterTheDeadlineToreTheTransportDownFailsInsteadOfThrowing`.
- **The send turn is RAII.** `ClientChannel::Send` holds it through
  `SendTurnGuard`, so a throw costs one request, not the channel. Pinned by
  `ClientChannelTimeoutTest.AThrowingSendReleasesTheSendTurn`.

**`CoSpawn` still swallows the exception.** A throw in a detached request
coroutine is silent; whether to log or terminate is an open policy question.

**Connect is a second exception, and unlike Publish it is not a deliberate
one.** `Call`'s deadline does not cover establishing a channel
(`ClientProtocolSession::Create` → `connection_.Open()` →
`transport_.Connect()` + `secure_channel_.Open()`).

**Both handshake halves are now bounded.** `ClientTransportContext` carries
`handshake_timeout` (`kDefaultHandshakeTimeout`, 30 s, `std::nullopt` to wait
indefinitely), applied by `ClientTransport::ReadFrameWithin()` — used by
`Connect()` for the Acknowledge and by `ClientSecureChannel` for the
OpenSecureChannel response. Keep it a separate constant from
`kDefaultClientRequestTimeout`.

**The deadline lives on the transport, not in its callers, and that placement is
forced.** Cancelling a read means destroying the transport, which only its owner
may do. Handshakes that run before `ClientChannel` exists use
`ReadFrameWithin()`.

**`Renew` rides on the same read, which matters more than it looks.**
`ClientChannel::Send` awaits `RenewSecurityToken()` before `Receive` arms its
deadline, so renewal must stay bounded by the handshake timeout.

**How it cancels is the part worth reading before editing it.** Destroying the
transport is how this codebase cancels an in-flight operation: the handler calls
`transport_.reset()`, the parked read fails and unwinds. Only the transport is
destroyed — never the owner. Report `Bad_Timeout`, not the read's
`Bad_NoCommunication`.

Pinned by `ClientTransportConnectTimeoutTest` in
`opcua/transport/binary/client_transport_unittest.cpp` (its
`SilentStreamTransport` fake answers scripted frames, then never again).

**What is still unbounded: the `transport_.open()` call itself** — the TCP
connect, bounded only by the OS (`ETIMEDOUT`). Known, not to be fixed here.

**Why this matters beyond the contract.** An unbounded connect is silent by
construction; for reproducers with a silent peer, copy the `SilentConnection`
fixture in `opcua/client/client_channel_unittest.cpp`.

Three invariants the implementation depends on, easy to break while editing:

- **`AsyncCompletion` is one-shot, and the deadline never settles it.**
  `Receive`'s deadline is `AsyncCompletion::WaitFor`
  (`opcua/base/async_completion.h`), which releases the waiter and leaves the
  gate open; only `DeliverResponse` and `FailPendingResponses` `Complete()` a
  pending entry. Keep their `completed()` guards.
- **Every bounded wait arms a real `steady_timer`, and a test only sees it
  settle if the executor owns that timer's service.** `TestExecutor`
  (`opcua/base/test/test_executor.h`) holds an `io_context` that
  `HasReadyTasks()` and `Advance()` poll. A test driving this code on another
  executor must poll that executor's timer service too. An *instant*
  `TestExecutor` keeps a self-serviced context, because nothing polls it.
- **A timed-out request is abandoned, not cancelled.** The peer may still
  answer; `abandoned_responses_` drops the late answer so it is not retained
  until the channel dies.

## A torn-down session stack outlives the coroutines parked on it

`ClientSession` holds its transport, secure channel, connection, channel and
protocol session as one `ClientSession::Stack`, **shared**, not owned outright.
`Reset()` — from `Disconnect`, `Reconnect`, and a `Connect` over a live stack —
aborts the stack's transport (`ClientTransport::Abort()`) and drops its
reference; it does not free the stack. Aborted coroutines resume on the stack to
unwind, so each holds a reference until done:

- the read loop, through `ClientChannel::Context::lifetime`, which `EnsureReadLoop`
  locks into the detached coroutine's closure;
- every `ClientSession` service method, through a local `shared_ptr<Stack>`;
- `ClientSubscription`, through `stack_lifetime_`, for its publish loop and its
  monitored-item coroutines.

Freeing it synchronously is a use-after-free; **the tell is a channel warning
whose `Endpoint` prints as garbage bytes**. Pinned by
`ClientSessionTest.ReconnectingUnderAnInFlightRequestKeepsItsStackAlive`, which
also fails if the stack leaks.

**When you add a coroutine that runs on the stack, give it a reference.** A raw
`ClientChannel&` or `this` that survives a `co_await` is the crashing shape.
Callbacks the stack invokes capture the session weakly (`on_stream_failed`), and
`~ClientSession` aborts the transport so a parked read unwinds.

## Every `ClientChannel` warning names its peer

`ClientChannel::Context` carries `endpoint_url`, and every `LOG_WARNING` the
channel emits ends with `LOG_TAG("Endpoint", endpoint_url_)`. `ClientSession`
passes the URL it connected to; a channel built without one logs an empty tag,
so the line shape is stable. The tag is load-bearing: when a downstream stops
draining, these channel warnings are the only lines the process emits about it.

**Add the tag to any new warning in this file**, and assert on it per line:
`ClientChannelTest.ReadFailureWarningNamesTheEndpoint` and
`.RenewalFailureWarningNamesTheEndpoint`
(`opcua/transport/binary/client_channel_unittest.cpp`) capture the log with
`opcua/base/test/scoped_log_capture.h` and check the line carrying the message,
not the whole capture. The capture helper is a copy of the framework's
(`scada-server-framework/base/test/
scoped_log_capture.h`), kept in step by hand, because the framework consumes
opcuapp and the reverse include would be a cycle.
