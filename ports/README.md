# vcpkg overlay ports

Status: Living reference
Last verified against code: 2026-08-15

Local overrides of upstream vcpkg ports. **An overlay added here reaches every
product automatically — there is nothing to wire up per product, and nothing to
add to a second file.**

That is worth stating plainly, because the arrangement it replaced failed
silently. vcpkg reads `overlay-ports` from the `vcpkg-configuration.json` beside
the *manifest*, not beside the source root, and until ADR 0011 phase 6 there was
one manifest — the repo root's — with the config next to it. Phase 6 gave every
product its own `vcpkg.json`, so every product became its own manifest root and
the root config was beside none of them. All 25 quietly resolved against
unpatched upstream ports for a week: nothing failed, because an overlay's
absence is not an error, and the only visible trace was a missing patch name in
`vcpkg_installed/<triplet>/share/<port>/vcpkg_abi_info.txt`.

What carries the overlay now is `scada_product_prologue()`
(`build-support/ScadaProductBase.cmake`), which runs before `project()` — and so
before the vcpkg toolchain — and appends `<search root>/ports` to
`VCPKG_OVERLAY_PORTS`. The search root is the tree root in the monorepo and the
product root in an export, and `ports/` sits directly beneath it in both
(`tools/export/products.toml` gives every code product its own copy). One
statement of the path, both layouts, every product.
`scada_overlay_ports_check` (ctest, at the repo root) pins it.

Two consequences to know about:

- **A `vcpkg-configuration.json` is no longer how an overlay reaches a build.**
  The repo root still has one, and so does `gcp/free-tier/build/` (the Linux
  cross build points `-DVCPKG_MANIFEST_DIR` at it). Both are now belt-and-braces
  — the prologue covers those configures too, and vcpkg accepts the same
  directory listed twice. Leave them; do not treat them as the place to add the
  next overlay.
- **`ports/` must exist or not be named at all.** vcpkg rejects an overlay path
  that is not an existing directory, which is why the prologue guards on
  `IS_DIRECTORY`. Deleting the last overlay therefore means deleting this whole
  directory, and no other edit.

To check whether a given build really got an overlay, read the port's
`vcpkg_abi_info.txt` in that build tree's `vcpkg_installed` — it lists the patch
files by name. Version numbers prove nothing here: the overlays are copied from
the same upstream version they override.

**Port files are stored verbatim, line endings included.** `.gitattributes` here
marks everything under a port directory `-text`, exempting it from the repo-wide
`* text=auto eol=lf`. Thirteen files in the qtbase port are CRLF upstream, and
normalizing them would make `diff -r` against the vcpkg checkout report
whole-file differences rather than the handful of lines we actually changed —
which is the whole re-sync workflow. The glob covers new port directories
automatically; no per-port line is needed.

**Everything here is temporary by construction.** An overlay port is a patch we
carry because upstream has not taken the fix yet. Each entry below records the
exact condition under which it must be deleted. When that condition is met,
delete the port directory — and, if it was the last one, this whole directory
rather than carrying it forward. Nothing else has to change: the prologue stops
naming a `ports/` that is not there.

## opentelemetry-cpp

Copied verbatim from vcpkg `f3e10653cc` (opentelemetry-cpp 1.27.0 — the version
our pinned vcpkg baseline resolves to) plus two added patches. Keeping the copy
byte-identical apart from those patches is deliberate: it keeps the diff against
upstream reviewable and makes re-syncing on a vcpkg bump mechanical.

Both patches fix the same class of defect: a `condition_variable` notifier that
does not hold the waiter's mutex, so a wakeup issued while the worker is inside
its wait predicate is lost and the worker sleeps out its full interval.

### `restore-metric-reader-shutdown-lock.patch`

`PeriodicExportingMetricReader::OnShutDown` calls `cv_.notify_all()` without
holding `cv_m_`, while the worker thread holds `cv_m_` while evaluating its wait
predicate. If the shutdown store and notify land between the predicate returning
false and the worker atomically releasing the mutex to block, the notification is
lost and the worker sleeps out its full remaining export interval. `join()` waits
that out, so shutdown stalls for one whole export interval.

Impact: with the deployed tiers' 60 s `export_interval_ms` this is a shutdown
stall of up to a minute; with the one-hour interval used in
`core/metrics/otel_metrics_unittest.cpp` it presents as `scada_metrics_unittests`
hanging forever (observed roughly 1 run in 3–20 on macOS).

The patch is a straight restoration of upstream commit `4f32bc6f`
([PR #2553](https://github.com/open-telemetry/opentelemetry-cpp/pull/2553),
merged 2024-05-08), which was reverted incidentally 19 days later by `605c3e8e`
([PR #2584](https://github.com/open-telemetry/opentelemetry-cpp/pull/2584),
"[SDK] Fix forceflush may wait for ever"). That PR was fixing the force-flush
path, which uses the separate `force_flush_m_`/`force_flush_cv_` pair, so
restoring the `cv_m_` lock does not undo it.

**Drop this overlay when:** a vcpkg baseline we upgrade to ships an
opentelemetry-cpp release whose
`sdk/src/metrics/export/periodic_exporting_metric_reader.cc` holds `cv_m_`
around the `cv_.notify_all()` in `OnShutDown`. Absent upstream as of v1.28.0 and
`main` (checked 2026-07-25). Check before assuming a version bump fixes it — a
newer release alone is not evidence, since the regression has survived every
release since 2024-05.

### `fix-batch-span-processor-lost-wakeup.patch`

The same defect in `BatchSpanProcessor`, at all four sites that signal a
condition variable — none of them holds the mutex the waiter evaluates its
predicate under, so a notification issued inside the predicate window reaches no
waiter and is lost. It affects **both directions** of the force-flush handshake:

1. **Worker wakeup** (`cv` / `cv_m`) — `ForceFlush`'s `break_condition` and
   `InternalShutdown` set `is_force_wakeup_background_worker` and notify without
   `cv_m`. A lost wakeup makes the worker sleep its whole `schedule_delay_millis`
   (5 s by default).
2. **Flush completion** (`force_flush_cv` / `force_flush_cv_m`) —
   `NotifyCompletion` publishes `force_flush_notified_sequence` and notifies
   without `force_flush_cv_m`, while `ForceFlush` reads that sequence from its
   predicate under that mutex. A lost wakeup makes `ForceFlush` sleep its whole
   wait quantum, which is also `schedule_delay_millis`.

Direction 1 is the damaging one: it makes `ForceFlush` return the **wrong
answer**, not merely late. `ForceFlush`'s retry loop never gets a second
iteration — its inner wait quantum is `schedule_delay_millis_` (5 s) and our
callers pass a 5 s timeout (`core/metrics/otel_traces.cpp`), so
`break_condition` runs once and the deadline expires at the same instant the
worker finally times out and exports. `ForceFlush` returns `false` having
exported nothing.

Impact: this is the intermittent `OtelTraceSinkTest` failure — a test ends
spans, calls `ForceFlush(5s)` and asserts on the batch, but sporadically sees
**zero spans**, with the test taking ~5005 ms instead of ~0 ms. Cases that
*expect* zero spans stall for 5 s and still pass, which is why the symptom looked
like an unrelated flake and why the failing test name changed from run to run.

Measured on macOS 2026-07-25, counting runs of that suite where some test took
≥ 1000 ms:

| build | stalled runs |
|---|---|
| unpatched | 19 / 100 |
| direction 1 only | 14 / 200 — all still *passing* (correct result, 5 s late) |
| both directions | 0 / 200 |

Fixing only direction 1 converts the failures into slow passes; the residual
stalls were direction 2. That is worth knowing if the patch is ever partially
re-synced.

The fix takes the waiter's mutex around each store-and-notify. Doing that at all
four sites needs one supporting change, because the naive version deadlocks:
`break_condition` runs under `force_flush_cv_m` and now takes `cv_m`, so the
reverse edge `cv_m` → `force_flush_cv_m` must not exist — and it otherwise would,
since the worker held `cv_m` across `Export()` → `NotifyCompletion()`.
`DoBackgroundWork` therefore releases `cv_m` once the predicate has been
consumed, before `Export()`/`DrainQueue()`. Nothing below that point needs it:
`cv_m` guards only the wait predicate, whose inputs (the `is_*` flags and the
lock-free `buffer_`) are atomic. The resulting order has no cycle:

```
force_flush_cv_m -> cv_m   (ForceFlush's break_condition)
shutdown_m       -> cv_m   (InternalShutdown)
force_flush_cv_m           (NotifyCompletion, holding nothing else)
```

**If you touch this patch, re-check that ordering.** Taking `force_flush_cv_m`
in `NotifyCompletion` is only safe *because* the worker no longer holds `cv_m`
during the export; reinstating the wider `cv_m` scope without dropping the
`NotifyCompletion` lock reintroduces an AB-BA deadlock. `NotifyCompletion` also
deliberately does not hold `force_flush_cv_m` across `exporter->ForceFlush()`,
which may block for a long time.

The worker's wait predicate additionally now checks `is_shutdown`, which it never
did despite the loop body handling `is_shutdown` immediately after the wait. And
with the notifiers holding `cv_m`, the worker's unconditional
`is_force_wakeup_background_worker.store(false)` after `wait_for` can no longer
swallow a concurrent force-wakeup, since setting that flag now requires `cv_m`.

Unlike the metric-reader patch this is **not** a restoration of a reverted
commit — it is a new fix, so there is no upstream PR to point at yet.

**Drop this overlay when:** a vcpkg baseline we upgrade to ships an
opentelemetry-cpp release whose `sdk/src/trace/batch_span_processor.cc` holds the
waiter's mutex around the notify in `ForceFlush`'s `break_condition`,
`InternalShutdown` and `NotifyCompletion`. Absent upstream as of v1.27.0, v1.28.0
and `main` (checked 2026-07-25).

### Known related defect, deliberately not patched here

`CollectAndExportOnce` notifies `force_flush_cv_` without holding
`force_flush_m_` (`periodic_exporting_metric_reader.cc:205`) — the same
notify-outside-the-lock shape, on the *metrics* force-flush path. By analogy
with direction 2 above it should cost a stalled metrics force-flush rather than
a wrong result, but it has not been root-caused against an observed failure, so
it is left alone rather than patched speculatively. It is a candidate for the
same upstream PR as the other two.

## qtbase

Copied verbatim from vcpkg `f3e10653cc` (qtbase 6.11.1) plus a two-line addition
to `cmake/qt_install_submodule.cmake` — no patch file, because the changed file
belongs to the port rather than to the upstream source. Same rule as above:
keeping the rest byte-identical is what makes a re-sync on a vcpkg bump
mechanical (`diff -r` against the upstream port should report that one file and
nothing else).

Without it, **every Qt *submodule* port hard-fails on a Mac that has only the
Command Line Tools** — `qtsvg`, `qttranslations` and `qttools` all die during
configure with

```
CMake Error: Can't determine Xcode version. Is Xcode installed?
```

`qtbase` itself builds, which is what makes this confusing: its own portfile
already passes `-DQT_NO_XCODE_MIN_VERSION_CHECK:BOOL=ON`, carrying the comment
"The cmd line tools are missing xcodebuild" (`portfile.cmake`). The shared helper
that every submodule port funnels through passes only
`QT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK`, and that flag does not cover this
check: in Qt's `QtPublicAppleHelpers.cmake` it demotes the SDK-version and
Xcode-version *comparisons* to warnings via `${message_type}`, but the
"can't determine the version at all" branch is an unconditional `FATAL_ERROR`
guarded solely by `if(NOT QT_NO_XCODE_MIN_VERSION_CHECK)`. So the overlay adds
that flag to the helper, mirroring what `portfile.cmake` does — including
listing the variable in `MAYBE_UNUSED_VARIABLES`, since it is Apple-only and
would otherwise warn on every other platform.

This is not a baseline-pin problem and bumping vcpkg will not fix it by itself.

Fixing the helper rather than the three leaf ports is deliberate: it is where the
defect is, it covers any Qt submodule port added later, and it is one overlay to
re-sync instead of three. The cost is that `qtbase`'s ABI hash changes, so
`qtbase` and every Qt submodule rebuild from source once — vcpkg hashes the whole
port directory and does not know this file is only copied into
`share/qtbase/` at install time.

**Drop this overlay when:** a vcpkg baseline we upgrade to passes
`QT_NO_XCODE_MIN_VERSION_CHECK` (or otherwise stops the Xcode-version check from
being fatal) inside `ports/qtbase/cmake/qt_install_submodule.cmake`, not merely
in `ports/qtbase/portfile.cmake` — the latter has done so for a long time and is
not what is missing. Absent as of `f3e10653cc` (checked 2026-08-08). Installing
full Xcode on the affected machine also makes it moot, but the overlay is what
keeps a Command-Line-Tools-only Mac able to build the client from a clean tree.
