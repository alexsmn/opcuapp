#!/usr/bin/env python3
"""Vendor the OPC UA stack out of the SCADA monorepo into opcuapp.

Copies, with their transitive in-`core`/`common` include closure:
  - core/base, core/scada, core/metrics headers (+ .cpp companions)
  - common/common/{data_services_util,coroutine_service_resolver}.h
  - the native common/opcua sources (root + binary/ + websocket/ + test/)

Destination layout (single tree, include root = repo root so "opcua/..."
resolves):
  opcua/base/...      vendored core/base
  opcua/scada/...     vendored core/scada
  opcua/metrics/...   vendored core/metrics
  opcua/common/...    vendored ScadaCommon helpers
  opcua/...           native sources (paths unchanged; they already self-
                      reference via the "opcua/" prefix)

This script only COPIES verbatim; the namespace / include-path codemod is a
separate pass (codemod.py).
"""
import os, re, shutil, collections, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OPCUAPP = os.path.dirname(HERE)                 # third_party/opcuapp
SCADA = os.path.dirname(os.path.dirname(OPCUAPP))  # repo root
CORE = os.path.join(SCADA, "core")
COMMON = os.path.join(SCADA, "common")
NATIVE = os.path.join(COMMON, "opcua")
DST = os.path.join(OPCUAPP, "opcua")

inc_re = re.compile(r'#include\s+"([^"]+)"')

# Seed = the direct, non-test, non-mock core/common includes used by the
# native opcua sources (computed from grep; see plan).
SEEDS = """
base/any_executor_dispatch.h base/any_executor.h base/async_completion.h
base/awaitable.h base/boost_log.h base/debug_util.h base/no_destructor.h
base/time_utils.h base/time/time.h base/utf_convert.h
scada/attribute_service.h scada/authentication_adapters.h scada/authentication.h
scada/basic_types.h scada/coroutine_services.h scada/data_services_factory.h
scada/data_services.h scada/data_value.h scada/date_time.h scada/event_util.h
scada/event.h scada/expanded_node_id.h scada/history_service.h
scada/history_types.h scada/item_factory_subscription.h
scada/legacy_monitored_item_adapter.h scada/localized_text.h
scada/method_service.h scada/monitored_item_service.h scada/monitored_item.h
scada/monitoring_parameters.h scada/node_id.h scada/node_management_service.h
scada/qualified_name.h scada/read_value_id.h scada/service_context.h
scada/session_service.h scada/standard_node_ids.h scada/status_or.h
scada/status.h scada/variant.h scada/view_service.h
net/net_executor_adapter.h
base/test/scoped_mock_clock_override.h
""".split()

# ScadaCommon helpers (live under common/<path>), vendored to opcua/common/.
COMMON_SEEDS = ["common/data_services_util.h", "common/coroutine_service_resolver.h"]

# Test-only helpers (mocks + fixtures + matchers) needed by the migrated
# *_unittest.cpp. Their transitive closure is vendored alongside the library
# headers so the test suite builds standalone.
TEST_SEEDS = """
base/test/awaitable_test.h base/test/test_executor.h
scada/attribute_service_mock.h scada/history_service_mock.h
scada/method_service_mock.h scada/monitored_item_service_mock.h
scada/node_management_service_mock.h scada/view_service_mock.h
scada/test/status_matchers.h scada/test/test_monitored_item.h
""".split()


def resolve(inc, roots):
    for root in roots:
        p = os.path.join(root, inc)
        if os.path.isfile(p):
            return p
    return None


def scan_includes(path, q):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = inc_re.search(line)
            if m:
                q.append(m.group(1))


def closure(seeds, roots):
    """BFS the include closure. For every header pulled in, also follow the
    includes of its `.cpp` companion — the implementation files drag in extra
    headers (e.g. base/format.h) that the header alone does not."""
    seen, q = set(), collections.deque(seeds)
    while q:
        inc = q.popleft()
        if inc in seen:
            continue
        p = resolve(inc, roots)
        if not p:
            continue
        seen.add(inc)
        scan_includes(p, q)
        if inc.endswith(".h"):
            cpp = resolve(inc[:-2] + ".cpp", roots)
            if cpp:
                scan_includes(cpp, q)
    return seen


def copy_one(src_root, rel, dst_subdir):
    """Copy header `rel` (and its .cpp companion) from src_root into
    DST/<dst_subdir>/<rel>."""
    n = 0
    for ext_rel in (rel, rel[:-2] + ".cpp" if rel.endswith(".h") else None):
        if not ext_rel:
            continue
        src = os.path.join(src_root, ext_rel)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(DST, dst_subdir, ext_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        n += 1
    return n


def main():
    if os.path.isdir(DST):
        shutil.rmtree(DST)
    os.makedirs(DST)

    # 1. core closure (base/scada/metrics) -> opcua/<ns>/...
    core_closure = closure(SEEDS, [CORE])
    nfiles = 0
    for rel in sorted(core_closure):
        nfiles += copy_one(CORE, rel, "")  # rel already starts with base/ scada/ metrics/
    print(f"core closure: {len(core_closure)} headers, {nfiles} files copied")

    # 1b. test helpers (mocks/fixtures) + their closure -> opcua/<ns>/...
    test_closure = closure(TEST_SEEDS, [CORE]) - core_closure
    tfiles = 0
    for rel in sorted(test_closure):
        tfiles += copy_one(CORE, rel, "")
    print(f"test closure: {len(test_closure)} new headers, {tfiles} files copied")

    # 1c. platform-split .cpp that are compiled (not #included), so the include
    # closure never reaches them. CMake selects the right one per platform.
    extra = 0
    for rel in ["base/time/time_posix.cpp", "base/time/time_win.cpp"]:
        src = os.path.join(CORE, rel)
        if os.path.isfile(src):
            dst = os.path.join(DST, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            extra += 1
    print(f"platform extras: {extra} files copied")

    # 2. common helpers -> opcua/common/...
    common_closure = closure(COMMON_SEEDS, [COMMON])
    # keep only the common/ ones (their scada/base deps are already vendored)
    cfiles = 0
    for rel in sorted(common_closure):
        if rel.startswith("common/"):
            cfiles += copy_one(COMMON, rel, "")
    print(f"common helpers: {cfiles} files copied")

    # 3. native opcua sources -> opcua/... (skip CMakeLists.txt; keep everything else)
    nf = 0
    for dirpath, _dirs, files in os.walk(NATIVE):
        for fn in files:
            if fn == "CMakeLists.txt":
                continue
            if not fn.endswith((".h", ".cpp", ".md")):
                continue
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, NATIVE)
            dst = os.path.join(DST, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            nf += 1
    print(f"native opcua sources: {nf} files copied")
    print("done.")


if __name__ == "__main__":
    main()
