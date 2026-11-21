# vcpkg overlay ports

Status: Living reference
Last verified against code: 2026-11-21 (boost-locale overlay added);
2026-11-15 (open62541 overlay added);
2026-11-07 (vcpkg bumped to a2f9862da6: qtbase
re-cut, opentelemetry-cpp dropped; 2026-08-16 for the rest)

<!-- doc-citations: external sdk/ include/boost/ libs/locale/ -->
<!-- `sdk/...` in this document is opentelemetry-cpp's own tree, named so a
     reader can find the upstream code an overlay patches. It is not in this
     repository and must not be repointed at anything that is. -->

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
  The repo root still has one, and so does `gcp/free-tier/build/`. Both are now
  belt-and-braces — the prologue covers those configures too, and vcpkg accepts
  the same directory listed twice. Leave them; do not treat them as the place to
  add the next overlay. Whether either was still worth keeping was asked and
  **settled on 2026-08-31: both stay.** The backlog entry that asked is gone,
  so a later reader finding these two files redundant is re-deriving a
  question that has an answer — this paragraph is it.

  **`gcp/free-tier/build/` is not a manifest directory**, whatever its
  `vcpkg.json` used to suggest — that file was deleted on 2026-08-16 and
  `build-package.sh` passes no `VCPKG_MANIFEST_DIR` at all. The directory is
  reached as `-DVCPKG_OVERLAY_TRIPLETS`, for the `*.cmake` triplets in it, and
  `x86_64-linux-gnu.toolchain.cmake` is copied out of it by path. So a
  `vcpkg-configuration.json` there is beside no manifest and can reach no
  build's overlay — which is a sharper reason to leave it alone than
  belt-and-braces.
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
`* text=auto eol=lf`. Seven files in the qtbase port are CRLF upstream (thirteen
until the 2026-11-07 vcpkg bump, when upstream converted six), and normalizing them would make `diff -r` against the vcpkg checkout report
whole-file differences rather than the handful of lines we actually changed —
which is the whole re-sync workflow. The glob covers new port directories
automatically; no per-port line is needed.

**Everything here is temporary by construction.** An overlay port is a patch we
carry because upstream has not taken the fix yet. Each entry below records the
exact condition under which it must be deleted. When that condition is met,
delete the port directory — and, if it was the last one, this whole directory
rather than carrying it forward. Nothing else has to change: the prologue stops
naming a `ports/` that is not there.

## opentelemetry-cpp — dropped 2026-11-07

Carried from 2026-07-25 until the vcpkg bump to `a2f9862da6` (2026-11-07),
which brings opentelemetry-cpp 1.29.0. Both patches it held fixed
a `condition_variable` notifier that did not hold the waiter's mutex, so a
wakeup was lost and the worker slept out its whole interval. Upstream 1.29.0
fixes both, so the overlay was deleted rather than re-synced. Checked against
the v1.29.0 source, not the version number (2026-11-07):

- **`restore-metric-reader-shutdown-lock.patch`** — a `MeterProvider` shutdown
  stall of one export interval; `scada_metrics_unittests` hanging forever. In
  [v1.29.0](https://raw.githubusercontent.com/open-telemetry/opentelemetry-cpp/v1.29.0/sdk/src/metrics/export/periodic_exporting_metric_reader.cc)
  `OnShutDown` stores `is_stop_requested_` under `cv_m_` before notifying,
  which closes the same window the restored lock did. v1.28.0 still notified
  bare.
- **`fix-batch-span-processor-lost-wakeup.patch`** — `OtelTraceSinkTest`
  sporadically seeing zero spans after a ~5 s `ForceFlush`. In
  [v1.29.0](https://raw.githubusercontent.com/open-telemetry/opentelemetry-cpp/v1.29.0/sdk/src/trace/batch_span_processor.cc)
  `ForceFlush`'s `break_condition` and `Shutdown` take `cv_m` around the
  store-and-notify, `NotifyCompletion` holds `force_flush_cv_m` around
  publishing the sequence, and `DoBackgroundWork` releases `cv_m` before
  `Export()` — the same lock order the patch introduced. Upstream's worker
  predicate does not check `is_shutdown`, which the patch added; it does not
  need to, since shutdown now sets the force-wakeup flag under `cv_m`.

The patches themselves are in `git log -- ports/opentelemetry-cpp`, along with
the measurements (19/100 stalled runs unpatched, 0/200 patched).

**If either symptom comes back**, it is this shape again, so look there first.
One instance was never patched and is **still upstream at v1.29.0**:
`CollectAndExportOnce` publishes `force_flush_notified_sequence_` and notifies
`force_flush_cv_` without `force_flush_m_`, while `OnForceFlush` waits on that
sequence under `force_flush_m_`. By analogy it should cost a late metrics
force-flush rather than a wrong result, and it has never been tied to an
observed failure, so it was left alone rather than patched speculatively.

## qtbase

Copied verbatim from vcpkg `a2f9862da6` (qtbase 6.11.2) plus a
two-line addition
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
not what is missing. Absent as of `f3e10653cc` (checked 2026-08-08) and again
at `a2f9862da6` (checked 2026-11-07, when the overlay was
re-cut from that tag). Installing
full Xcode on the affected machine also makes it moot, but the overlay is what
keeps a Command-Line-Tools-only Mac able to build the client from a clean tree.

## open62541

Copied verbatim from vcpkg `a2f9862da6` (open62541 1.4.21), plus one feature,
`alarms-conditions`, in `vcpkg.json` and `portfile.cmake`. Nothing else differs
(`diff -r` against the upstream port shows those two files only). Added
2026-11-15 (backlog 1055), with the user's approval, as a dependency decision.

**Only the superproject root asks for the feature**, for the
`foreign_server_e2e_tests` fixture (`test/e2e/foreign_server/`). It is the only
manifest in the tree that names open62541 at all, so no product build uses this
port. It still travels in every export, because every code product claims
`ports/` whole. That is harmless — an overlay for a package a product does not
depend on is never read — but it does mean the published client, core and
common repositories carry it.

**What it enables.** open62541's Alarms & Conditions
(`UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS`), which the fixture needs to serve
a real Condition. Without it, a foreign server's events carry no ConditionId,
and the client's Part 9 Acknowledge could only be tested by handing it a
target. open62541 refuses A&C without events and the full namespace zero
([CMakeLists.txt at v1.4.21](https://raw.githubusercontent.com/open62541/open62541/v1.4.21/CMakeLists.txt),
the `FATAL_ERROR` "Unable to enable A&C without UA_ENABLE_SUBSCRIPTIONS_EVENTS
and full namespace 0"), so the feature also brings `subscriptions-events` and
`-DUA_NAMESPACE_ZERO=FULL`. Upstream marks A&C EXPERIMENTAL and prints a
configure warning saying so.

**The full namespace zero needs a second source.** It is generated from the
OPC Foundation's `UA-Nodeset`, which open62541 carries as the `deps/ua-nodeset`
git submodule. The release tarball `vcpkg_from_github` fetches does not include
it, and open62541's configure then tries `git submodule update` and fails. The
portfile therefore fetches `OPCFoundation/UA-Nodeset` at
`d1bb6a22125bd7cd986272b1ee98a18a91d76fff`, the commit v1.4.21 pins
([GitHub contents API at v1.4.21](https://api.github.com/repos/open62541/open62541/contents/deps/ua-nodeset?ref=v1.4.21),
verified 2026-11-15), and passes it as `UA_NODESET_DIR`, which is a cache
variable in open62541's CMake. **On a version bump, re-read that pin**:
a different open62541 pins a different nodeset commit, and a mismatched
nodeset fails the namespace-zero generation rather than reporting itself.

**Cost.** The full namespace zero makes open62541 noticeably bigger and slower
to build than the reduced default. It is paid once per cache miss, and only by
a root configure.

**Drop this overlay when:** the upstream vcpkg port exposes Alarms & Conditions
as a feature (with the full namespace zero it requires). Request that feature
from the root manifest instead, and delete this directory. Absent at
`a2f9862da6`
([portfile.cmake](https://raw.githubusercontent.com/microsoft/vcpkg/a2f9862da6d88040a8537fb9b8cb24deaa9a196b/ports/open62541/portfile.cmake)),
and on vcpkg `master`, checked 2026-11-15. A second way out is
for the fixture to stop needing a real Condition, which would also mean
dropping the end-to-end acknowledge test.

## boost-locale

Copied verbatim from vcpkg commit
[`5ad42ec00d`](https://github.com/microsoft/vcpkg/commit/5ad42ec00d)
("[boost-locale] Fix configuration with CMake 4.5",
[microsoft/vcpkg#55069](https://github.com/microsoft/vcpkg/pull/55069)), which
is our baseline `a2f9862da6`'s port (boost-locale 1.92.0) plus
`fix-source-group.patch` and `"port-version": 1`. Nothing else differs (`diff`
against the baseline port shows only those). Added 2026-11-21 for the 2.6.6
Client release.

**Why.** boost-locale 1.92.0's `libs/locale/CMakeLists.txt` passes
`include/boost/locale.hpp` to
`source_group(TREE ".../include/boost/locale" ...)`. CMake 4.4.5 accepts that
(the tree is a prefix of the file as a string); CMake 4.5.0 rejects it with
"is not a prefix of file" — checked 2026-11-21 with a three-line project under
both versions. The Client's v2.6.6 Windows CI run (runner image
`windows-2025-vs2026`, 2026-11-21) used CMake 4.5.0 and failed in
`vcpkg install` building boost-locale; the v2.6.5 run on 2026-11-19, from the
same baseline port, had configured it without error. Any configure that builds
boost-locale from source with CMake 4.5 fails the same way. Upstream boost-locale fixed its CMakeLists on 2026-10-25
([f51b58cd34](https://github.com/boostorg/locale/commit/f51b58cd34),
"cmake: use remove_item for locale.hpp"); the patch is that fix.

**Drop this overlay when:** the vcpkg baseline is moved to `5ad42ec00d` or
later (the upstream port then carries the patch), or to a Boost release whose
boost-locale includes f51b58cd34. Delete this directory and this section.
