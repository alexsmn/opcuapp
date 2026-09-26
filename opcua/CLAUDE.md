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
- `ua_json_codec.{h,cpp}` — the conformant OPC UA JSON encoding ([Part 6 §5.4
  "OPC UA JSON"](https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4)),
  in the compact form the published service schema describes. The built-in
  layer it bottoms out in is hand-written in `ua/ua_json_builtins.{h,cpp}`,
  mirroring how the binary codec bottoms out in `codec_utils.cpp`. Also
  `JsonEncodingId<T>` and `To/FromJsonExtensionObject`.

**The two ExtensionObject helper pairs are not interchangeable.** An
ExtensionObject body is either a binary ByteString keyed by the DefaultBinary
id, or JSON keyed by the DefaultJson id ([Part 6 §5.4.2.16
"ExtensionObject"](https://reference.opcfoundation.org/Core/Part6/v105/docs/5.4.2.16)).
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

**The SecureChannel handshake is not an exception, but its framing is.** The
message *bodies* — `OpenSecureChannelRequest`/`Response`,
`CloseSecureChannelRequest` — are generated types, aliased into
`opcua::binary` by `transport/binary/secure_channel.h`, and their bodies are
encoded by `ua::Encode`/`ua::Decode` keyed on each message's own
`kBinaryEncodingId`. What stays hand-written there is the
`SecureConversationMessage` and its asymmetric/symmetric security and sequence
headers: those are framing from [Part 6 §6.7 "OPC UA Secure
Conversation"](https://reference.opcfoundation.org/Core/Part6/v105/docs/6.7),
which defines the MessageChunk, security and sequence headers — not
StructuredTypes, so
the schema has nothing to generate for them.

They were hand-transcribed structs with their own `kOpen…EncodingId = 446`
constants until 2026-08-08, and the cost was concrete: the hand-written
ResponseHeader encoder wrote `service_result.good() ? 0 : 0x80000000`, so every
rejected OpenSecureChannel reached the client as a generic Bad with the reason
discarded. That is the failure shape a second, separately-maintained copy of a
message layout produces — it does not diverge loudly, it quietly drops a field's
meaning.

Note that `ua_encoding_ids.h` defines a name like `kOpenSecureChannelRequest`
**twice**, once in `binary_encoding_id` (446) and once in `json_encoding_id`
(15132). Prefer `T::kBinaryEncodingId` over either constant: it cannot be
paired with the wrong codec.

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

## Client requests are bounded; Publish is the exception

`ClientChannel::Call` applies a deadline (`kDefaultClientRequestTimeout`, 30 s,
overridable per channel via `Context::request_timeout`) and answers
`Bad_Timeout` if the peer does not. **`Receive` does not, and must not by
default** — it is also the split send/receive path Publish uses, and [Part 4
§5.14.5 Publish](https://reference.opcfoundation.org/Core/Part4/v105/docs/5.14.5)
has the server hold a Publish request until data is available, so a deadline
there would tear down healthy subscriptions rather than protect them. A caller
that wants a bounded `Receive` passes one explicitly.

**Why a deadline is needed at all, given the read loop reports failures.**
Nothing below this layer bounds a wait. The only thing that completes a pending
entry other than its response is `FailPendingResponses`, whose sole caller is
`RunReadLoop`'s failure path — so the wakeup depends on the transport raising an
error. Against a **half-open** connection (peer gone, socket still open) it
never does: the read loop sits in `ReadResponse()` waiting for bytes that never
arrive, and every caller waits for ever. A peer that is merely *down* was always
fine — it fails fast with `Bad_NoCommunication`. The failure needs
TCP-absent-but-believed-present, which is what a close-without-reconnect
produces.

This was not theoretical. On 2026-08-26 an aggregating proxy lost a downstream
and had not reconnected 48 minutes later, parked in a liveness `Probe()` that
is an un-deadlined Read; the same evening two protocol edges wedged while fully
connected, their last spans `opcua.client/Browse` — stuck in their own outbound
call.

**Do not "fix" a hang like that one layer up.** Bolting a timeout onto the
caller does not recover, because the recovery path's next step is itself an
un-deadlined request on the same dead connection: `ClientSession::Disconnect`
→ `ClientProtocolSession::Close` sends CloseSession before tearing anything
down. The wedge moves one line, it does not go away.

**This paragraph used to end "Bound the request here and both are covered."
That was wrong, and it was disproved in production on 2026-08-30.** A session
bounded the aggregating proxy's liveness `Probe()` one layer up — exactly what
the paragraph warns against — and the wedge duly moved one line down, to
`Disconnect()`, where a watchdog recorded it stuck for 840 s. But bounding the
request *here* would not have saved it either, because **`Call`'s deadline
covers only `Receive`**. The whole send half was unbounded:

- `WaitForSendTurn()` waits on an `AsyncCompletion` with no deadline;
- `SendRequest` → `SecureChannel::SendServiceRequest` → `ClientTransport::WriteFrame`
  → `write_queue_.Write` had none either;
- and `ClientConnection::Close`'s own CLO write goes down the same path.

So a peer that accepts bytes and stops draining parks the write for ever.
`ReleaseSendTurn()` runs only *after* the write returns, so every later request
on that channel then parks in `WaitForSendTurn` **before any deadline is
armed** — one stalled socket wedges the entire channel, not one request.

**The fix is `kDefaultWriteTimeout` (30 s) on `ClientTransport::WriteFrame`**,
applied with the same mechanism as the handshake read: the deadline handler
resets the *transport*, which fails the parked write and unwinds the coroutine
normally. Bounding the write also drains the send-turn queue, because the
failed send releases the turn — so one bound closes both halves.

**A write must never be abandoned in place.** A half-written frame left on the
wire is the desynchronised stream that makes a peer log `Undecodable or
unsupported secure-channel frame` and hang up, which is the failure that
started this whole line of work. Tearing the transport down is the only safe
answer to a write that did not finish, and it is why the deadline ends the
connection rather than returning the caller to a channel it could keep using.
`ClientTransportConnectTimeoutTest.WriteTimesOutWhenThePeerStopsDraining` pins
it and was confirmed to fail with the bound removed.

**That deadline then wedged the channel itself, and this was the `Disconnect`
stall the aggregating proxy suffered six times between 2026-08-30 and
2026-09-05** (superproject backlog 647, closed 2026-09-08). The handler resets
the transport, and `transport_` is null from then on — so the *next*
`WriteFrame` built its own deadline timer on `transport_.get_executor()`, which
on a reset `any_transport` is an **empty** `any_io_executor`, and both
`steady_timer` and the write queue's `experimental::channel` throw
`bad_executor` when constructed on one (reproduced 2026-09-08 with a 20-line
snippet against this tree's Boost 1.91). The throw escaped
`SecureChannel::SendServiceRequest`, then `ClientChannel::Send` between
`WaitForSendTurn` and `ReleaseSendTurn`, then the `co_spawn(..., detached)`
coroutine that owned the request — which drops the exception on the floor. So
the send turn was held for ever, and every later request on the channel parked
in `WaitForSendTurn` before any deadline was armed: the liveness probe first
(abandoned by the reconnect loop's 45 s bound rather than answering `Call`'s
30 s one, which is the tell in the logs), then the CloseSession that
`Disconnect()` sends, then every Browse the proxy fanned out to that
downstream. Two loops wedging within 4 ms of each other were two channels
timing out a write under the same CPU starvation.

Two fixes, each pinned by a test confirmed to fail without it:

- `ClientTransport::WriteFrame` and `ReadFrameWithin` refuse a torn-down
  transport with `Bad_NoCommunication` before touching the executor
  (`WriteAfterTheDeadlineToreTheTransportDownFailsInsteadOfThrowing`).
- `ClientChannel::Send` holds the turn through `SendTurnGuard`, an RAII
  release, so the next throw from whatever raises it costs one request rather
  than the channel (`ClientChannelTimeoutTest.AThrowingSendReleasesTheSendTurn`).

**`CoSpawn` still swallows the exception.** That silence is why the cause hid
for nine days behind a watchdog that named the step and not the throw; whether
the completion handler should log or terminate is a policy question spanning
this product and `core/`, filed in the superproject backlog.

**Connect is a second exception, and unlike Publish it is not a deliberate
one.** `Call`'s deadline covers a request on an *established* channel. It does
not cover establishing one: `ClientProtocolSession::Create` awaits
`connection_.Open()` before it issues CreateSession
(`opcua/client/client_protocol_session.cpp`), and `Open()` is
`transport_.Connect()` followed by `secure_channel_.Open()`
(`opcua/transport/binary/client_connection.cpp:13-19`) — neither of which goes
through `Call`. `ClientTransport::Connect` writes Hello and then awaits
`ReadFrame()` for the server's Acknowledge
(`opcua/transport/binary/client_transport.cpp:48`) with nothing bounding the
wait; `grep -c 'steady_timer\|expires_after'` over `client_transport.cpp` and
`secure_channel.cpp` returns **0** for both (verified 2026-08-26).

So a peer that completes the TCP handshake and then says nothing — accepting
the connection but never answering Hello — hung the *connect* for ever, and the
caller saw no error, no status and no log line, because nothing had been sent
through the deadline'd path yet. That is a different shape from the half-open
connection above, and `Call`'s 30 s deadline does not reach it.

**Both handshake halves are now bounded** (2026-08-26). `ClientTransportContext`
carries a `handshake_timeout` (`kDefaultHandshakeTimeout`, 30 s, `std::nullopt`
to wait indefinitely), and `ClientTransport::ReadFrameWithin()` applies it. Two
readers use it: `Connect()` for the Acknowledge, and `ClientSecureChannel` for
the OpenSecureChannel response. It is a separate constant from
`kDefaultClientRequestTimeout` on purpose: the two bound different things — a
service call on a live channel, and a handshake with a peer that may never have
been alive — and are free to diverge.

**The deadline lives on the transport, not in its callers, and that placement is
forced.** Cancelling the read means destroying the transport, so only its owner
can do it, and the owner must outlive the read it cancels. A deadline armed in
`ClientSecureChannel` — which holds the transport by reference — would have had
to reach through to destroy something it does not own. `ReadFrameWithin()` is
therefore the seam: the layer that owns the socket offers a bounded read, and
the handshakes that run before `ClientChannel` exists take it.

**`Renew` rides on the same read, which matters more than it looks.**
`ClientSecureChannel::OpenSecureChannel` serves both Issue and Renew, and
`ClientChannel::Send` awaits `RenewSecurityToken()` *before* `Receive` arms its
deadline — so an unbounded renewal parked a send that looked protected by the
30 s request timeout and was not.

**How it cancels is the part worth reading before editing it.** The read cannot
be asked to stop: `transport::any_transport`'s contract is that *destroying* the
transport is how this codebase cancels an operation in flight, and that a
pending read resumes touching only locals when it does. So the deadline handler
calls `transport_.reset()`, which fails the parked read and unwinds the
coroutine normally. Only the transport is destroyed — the `ClientTransport`
outlives the handler, so its own members stay valid, which is what makes the
documented mechanism safe *here* and would not be true of a handler that tore
down the owner. `Bad_Timeout` is reported rather than the read's own
`Bad_NoCommunication`, because the read failed by this handler's doing and
"unreachable" would be the wrong story about a peer that accepted the
connection.

Pinned by `ClientTransportConnectTimeoutTest` in
`opcua/transport/binary/client_transport_unittest.cpp`, whose
`SilentStreamTransport` fake answers scripted frames and then never answers
again until it is destroyed — which is the real contract
rather than a convenience.

**What is still unbounded: the `transport_.open()` call itself**, the TCP
connect at the top of `ClientTransport::Connect`. In practice the OS bounds it —
a SYN that goes unanswered fails with `ETIMEDOUT` — so it is not the silent-peer
shape this section is about, and nothing here overrides that. Worth knowing
rather than fixing: the guarantee is the platform's, not ours, and a host with
an unusually long `tcp_syn_retries` inherits it.

**Why this matters beyond the contract.** It is the standing explanation for a
symptom nobody has yet accounted for: a historian whose external collection
source stopped and whose later processes logged **no connect attempt at all**
(superproject backlog 547). `ExternalHistorySources::Connect` logs only on a
returned failure, so an unbounded connect is silent by construction. That is a
hypothesis rather than a diagnosis — the chain is verified by reading and the
effect is not, and confirming it wants a reproducer with a silent peer, not a
deployment. `opcua/client/client_channel_unittest.cpp`'s `SilentConnection`
fixture is the shape to copy, one layer down.

Two invariants the implementation depends on, easy to break while editing:

- **`AsyncCompletion` is one-shot, and the deadline never settles it.**
  `Receive`'s deadline is `AsyncCompletion::WaitFor`
  (`opcua/base/async_completion.h`), which releases the waiting coroutine and
  leaves the gate open, so the only parties that ever `Complete()` a pending
  entry are `DeliverResponse` and `FailPendingResponses`. Until 2026-09-05 the
  deadline was a hand-rolled `steady_timer` that completed the gate itself
  (superproject backlog 661), which is where the race — and the `completed()`
  guards both functions carry — came from; the guards stay as belt-and-braces.
  One consequence is visible: a response landing in the window between the
  deadline firing and `Receive` resuming used to be *buffered* for a caller
  that had gone (the gate was already complete, and the id was not yet in
  `abandoned_responses_`); it is now delivered into the pending entry and
  returned as the answer.
- **Every bounded wait arms a real `steady_timer`, and a test only sees it
  settle if the executor owns that timer's service.** `cancel_after` completes
  its operation only after the cancelled timer's handler has run, so even a
  `Call` answered at once finishes through the timer service. `TestExecutor`
  (`opcua/base/test/test_executor.h`) therefore holds an `io_context` that
  `HasReadyTasks()` and `Advance()` poll; until 2026-09-26 it held a bare
  `execution_context`, asio serviced the timer from a thread of its own, and
  `Drain()` returned while the continuation was still in flight —
  `ClientSessionTest.MonitoredItemLifecycleReachesTheWire` failed about half
  its repeated runs and a whole-binary repeat hung (superproject backlog
  730). A test that drives this code on a different executor must poll that
  executor's timer service too, or it inherits the same race. An *instant*
  `TestExecutor` is the exception and keeps a self-serviced context, because
  nothing polls it.
- **A timed-out request is abandoned, not cancelled.** The peer was never told
  and may still answer, so `abandoned_responses_` drops that late answer;
  without it the response is buffered for a `Receive` that will never come and
  is retained until the channel dies.

## Every `ClientChannel` warning names its peer

`ClientChannel::Context` carries `endpoint_url`, and every `LOG_WARNING` the
channel emits ends with `LOG_TAG("Endpoint", endpoint_url_)`. `ClientSession`
passes the URL it connected to; a channel built without one logs an empty tag
rather than none, so the shape of the line is stable.

The tag is load-bearing, not cosmetic. When a downstream stops draining its
socket the layer that owns the reconnect loop logs **nothing** — the link still
looks connected, so there is no probe failure and no reconnect line — and this
channel's `OPC UA response read failed` / `security-token renewal failed` are
the only lines the process emits about it. Superproject backlog 705 was four
days of exactly those two lines every five seconds, naming no peer, on a proxy
aggregating six downstreams; the stuck two had to be identified from the
proxy's socket table (`ss -tnop` inside its network namespace, 86 bytes stuck
in `Recv-Q`). The Aggregation-layer lines had gained an `Endpoint` tag for the
same reason a week earlier (backlog 647) and it did not help here, because the
failure surfaced one layer down.

**Add the tag to any new warning in this file**, and assert on it per line:
`ClientChannelTest.ReadFailureWarningNamesTheEndpoint` and
`.RenewalFailureWarningNamesTheEndpoint`
(`opcua/transport/binary/client_channel_unittest.cpp`) capture the log with
`opcua/base/test/scoped_log_capture.h` and check the line that carries the
message, not the capture as a whole — a tag on some other line must not
satisfy the check, which is how the framework's Aggregation tags first passed
wrongly. Both were confirmed to fail with the tag removed. The capture helper
is a copy of the framework's (`scada-server-framework/base/test/
scoped_log_capture.h`), kept in step by hand, because the framework consumes
opcuapp and the reverse include would be a cycle.
