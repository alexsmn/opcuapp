#pragma once

#include "opcua/base/boost_log.h"
#include "opcua/types/duration.h"
#include "opcua/types/status.h"

// One rule for how loudly a completed service call is logged, shared by the
// client session that issues the call and the server handler that serves it.
//
// A completed query is trace, not an event: Read fires once per polled item and
// Browse once per fan-out step, so on a deployed aggregating proxy the two were
// most of the whole process's log output, every sampled one Status = Good. They
// therefore log at debug -- present in the on-VM log file, absent from anything
// shipping stdout onward.
//
// Slow or failed calls stay at info, because the DurationMs on these lines is
// what identified two production stalls nothing else caught: a 25.7 s
// downstream hang (scada-server-framework/docs/license-expiry-shutdown-fix.md)
// and the saturated host behind a wedged aggregation link (backlog 647). The
// threshold sits far above normal -- a ten-minute production sample of Read ran
// p50 1 ms, p99 24 ms, max 56 ms -- so a healthy system emits none of these and
// a stall still speaks.
//
// This lives in one place on purpose. The rule was briefly applied to the
// serving side and only half of the calling side, and a client that is quiet
// while its server is loud (or the reverse) makes a fan-out impossible to read.
//
// Mutations are deliberately NOT routed through this. Write and Call stay at
// info unconditionally: they are rare, they change state, and they never
// appeared in the volume census.

namespace opcua {

// The threshold above which a successful call is still worth an info record.
inline constexpr Duration kSlowCallLogThreshold =
    Duration::FromMilliseconds(1000);

// Returns info when the call failed or was slow, debug otherwise.
inline BoostLogSeverity CallCompletionSeverity(Status status,
                                               Duration duration) {
  return status.bad() || duration >= kSlowCallLogThreshold
             ? BoostLogSeverity::info
             : BoostLogSeverity::debug;
}

}  // namespace opcua
