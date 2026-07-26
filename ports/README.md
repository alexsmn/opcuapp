# vcpkg overlay ports

Status: Living reference
Last verified against code: 2026-07-25

Local overrides of upstream vcpkg ports, declared via `overlay-ports` in
`vcpkg-configuration.json` so every preset and every developer machine picks
them up without extra wiring.

**There are two manifest roots, and each needs its own
`vcpkg-configuration.json`:**

| Manifest root | Used by | Overlay path |
|---|---|---|
| `vcpkg.json` (repo root) | all CMake presets (macOS, Windows) | `./ports` |
| `gcp/free-tier/build/vcpkg.json` | the Linux cross build in `gcp/free-tier/multitier/build-package.sh` (`-DVCPKG_MANIFEST_DIR=/src/gcp/free-tier/build`) | `../../../ports` |

vcpkg reads `vcpkg-configuration.json` from the *manifest* directory, not the
source root, so adding an overlay to only the repo-root config silently leaves
the deployed Linux tier binaries unpatched. If you add an overlay, add it to
both.

**Everything here is temporary by construction.** An overlay port is a patch we
carry because upstream has not taken the fix yet. Each entry below records the
exact condition under which it must be deleted. When that condition is met,
delete the port directory (and this whole directory plus
`vcpkg-configuration.json` if it was the last one) rather than carrying it
forward.

## opentelemetry-cpp

Copied verbatim from vcpkg `f3e10653cc` (opentelemetry-cpp 1.27.0 — the version
our pinned vcpkg baseline resolves to) plus one added patch. Keeping the copy
byte-identical apart from that patch is deliberate: it keeps the diff against
upstream reviewable and makes re-syncing on a vcpkg bump mechanical.

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

### Known related defect, deliberately not patched here

`CollectAndExportOnce` notifies `force_flush_cv_` without holding
`force_flush_m_` (`periodic_exporting_metric_reader.cc:205`), and
`BatchSpanProcessor::InternalShutdown` has the same notify-outside-the-lock shape
(its wait predicate does not check `is_shutdown` at all). The batch-span variant
is the suspected cause of intermittent `OtelTraceSinkTest` failures that report
zero spans after exactly 5005 ms — the processor's default 5 s schedule delay.
Neither is root-caused yet, so neither is patched; both are candidates for the
same upstream PR.
