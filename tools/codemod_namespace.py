#!/usr/bin/env python3
"""Namespace the vendored core/common sources so opcuapp's public symbols are
distinct from the SCADA monorepo's, while keeping the build (and C++ name
lookup) correct.

Strategy, by file class:

  GLOBAL_KEEP — low-level utilities that reference neither `base::` nor
    `scada::` and are used bare / `::`-qualified from anonymous-namespace
    helpers (UtfConvert, Format, debug_util operators, StructWriter,
    DebugHolder, TraceId, the net executor adapter, boost_log). In core these
    live at global scope; keep them there so existing references resolve
    unchanged. They carry no OPC-UA domain meaning, so leaving them global is
    acceptable (boost_log already had to stay global for its macros).

  CAT2_WRAP — the async-infra cluster (Awaitable, AnyExecutor, CoSpawn,
    Dispatch, Cancelation, CallbackAwaitable) plus time_utils (which uses
    base::). These are declared at global scope (or only `namespace internal`)
    but appear in opcuapp's PUBLIC API, so they MUST be distinct from core's to
    avoid ODR clashes in a Stage-C adapter TU. Wrap each file's declaration
    body in `namespace opcua { ... }`; native code (in namespace opcua) then
    resolves bare `Awaitable` etc. via enclosing-namespace lookup, and a nested
    `namespace internal` becomes opcua::internal. Global-qualified
    `::internal::` self-references are rewritten to relative `internal::`.

  CAT1 — every remaining vendored file that declares one of the target
    namespaces scada / base / data_services. Rewrite the namespace declaration
    (`namespace scada` -> `namespace opcua::scada`) AND qualified references
    (`scada::` -> `opcua::scada::`, `base::` -> `opcua::base::`). The reference
    rewrite keeps out-of-namespace blocks correct, notably a global
    `namespace std { struct hash<scada::NodeId> }` specialization.

Native sources (root / binary / websocket) are left as-is except that any
file-scope `namespace scada { ... }` forward-declaration block is rewritten to
`namespace opcua::scada` so it does not create a stray global `::scada`.
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
OPCUA = os.path.join(os.path.dirname(HERE), "opcua")
VENDORED_DIRS = ("base", "scada", "metrics", "common", "net")

# Keyed by path relative to opcua/ without extension; covers both .h and .cpp.
GLOBAL_KEEP = {
    "base/utf_convert", "base/format", "base/debug_util", "base/struct_writer",
    "base/debug_holder", "metrics/trace_id", "base/boost_log",
}
CAT2_WRAP = {
    "base/awaitable", "base/any_executor", "base/any_executor_dispatch",
    "base/callback_awaitable", "base/cancelation", "base/time_utils",
    "net/net_executor_adapter",
}

CAT1_NS_RE = re.compile(r'^namespace (scada|base|data_services)\b', re.M)
ns_decl_res = [
    (re.compile(r'\bnamespace scada\b(?!::)'), 'namespace opcua::scada'),
    (re.compile(r'\bnamespace base\b(?!::)'), 'namespace opcua::base'),
    (re.compile(r'\bnamespace data_services\b(?!::)'), 'namespace opcua::data_services'),
]
ref_res = [
    (re.compile(r'(?<![\w:])scada::'), 'opcua::scada::'),
    (re.compile(r'(?<![\w:])base::'), 'opcua::base::'),
]
OPEN = "namespace opcua {\n"
CLOSE = "}  // namespace opcua (vendored)\n"
preamble_re = re.compile(r'^﻿?\s*(#pragma|#include|//|/\*|\*/|\*|$)')
internal_ref_re = re.compile(r'(?<![\w:]):: *internal::')


def key_for(path):
    """Path relative to opcua/ without extension, e.g. 'base/awaitable'."""
    rel = os.path.relpath(path, OPCUA).replace(os.sep, "/")
    return rel[:-2] if rel.endswith(".h") else rel[:-4] if rel.endswith(".cpp") else rel


def transform_cat1(text):
    for rx, rep in ref_res:
        text = rx.sub(rep, text)
    for rx, rep in ns_decl_res:
        text = rx.sub(rep, text)
    return text


def transform_cat2(text):
    lines = [internal_ref_re.sub('internal::', ln)
             for ln in text.splitlines(keepends=True)]
    insert = next((i for i, ln in enumerate(lines) if not preamble_re.match(ln)), None)
    if insert is None:
        return text
    out = lines[:insert] + [OPEN] + lines[insert:]
    if out and not out[-1].endswith("\n"):
        out[-1] += "\n"
    out.append(CLOSE)
    return "".join(out)


def fixups():
    # services_factory.cpp declares a helper in an anonymous namespace at file
    # scope (outside namespace opcua), so its scada::/base::/Awaitable uses must
    # be fully qualified.
    sf = os.path.join(OPCUA, "services_factory.cpp")
    if os.path.isfile(sf):
        with open(sf, encoding="utf-8", errors="replace") as f:
            t = f.read()
        t = re.sub(r'(?<![\w:])scada::', 'opcua::scada::', t)
        t = re.sub(r'(?<![\w:])base::', 'opcua::base::', t)
        t = re.sub(r'(?<![\w:])Awaitable<', 'opcua::Awaitable<', t)
        with open(sf, "w", encoding="utf-8") as f:
            f.write(t)

    # data_services_factory.h declares the DataServicesContext struct and the
    # REGISTER_DATA_SERVICES macro at global scope (it forward-declares the real
    # `namespace transport`, so it can't be blanket-wrapped). Qualify its bare
    # AnyExecutor uses; it does not define AnyExecutor, so this is safe.
    dsf = os.path.join(OPCUA, "scada", "data_services_factory.h")
    if os.path.isfile(dsf):
        with open(dsf, encoding="utf-8", errors="replace") as f:
            t = f.read()
        t2 = re.sub(r'(?<![\w:])AnyExecutor\b', 'opcua::AnyExecutor', t)
        if t2 != t:
            with open(dsf, "w", encoding="utf-8") as f:
                f.write(t2)


def main():
    n1 = n2 = nk = 0
    for d in VENDORED_DIRS:
        root = os.path.join(OPCUA, d)
        if not os.path.isdir(root):
            continue
        for dirpath, _x, files in os.walk(root):
            for fn in files:
                if not fn.endswith((".h", ".cpp")):
                    continue
                p = os.path.join(dirpath, fn)
                k = key_for(p)
                if k in GLOBAL_KEEP:
                    nk += 1
                    continue
                with open(p, encoding="utf-8", errors="replace") as f:
                    text = f.read()
                if k in CAT2_WRAP:
                    new = transform_cat2(text)
                    if new != text:
                        with open(p, "w", encoding="utf-8") as f:
                            f.write(new)
                        n2 += 1
                else:
                    # Cat 1 for every remaining vendored file (not just those
                    # that DECLARE namespace scada/base — also .cpp files that
                    # merely USE scada::/base:: in global-scope helpers).
                    new = transform_cat1(text)
                    if new != text:
                        with open(p, "w", encoding="utf-8") as f:
                            f.write(new)
                        n1 += 1
    print(f"cat1 (nest scada/base/data_services): {n1} files")
    print(f"cat2 (wrap async/time_utils in opcua): {n2} files")
    print(f"kept global (deep utils): {nk} files")

    # Native forward-declaration blocks: rewrite `namespace scada { ... }` so it
    # does not shadow opcua::scada with a stray global ::scada.
    n3 = 0
    for dirpath, _x, files in os.walk(OPCUA):
        if os.path.relpath(dirpath, OPCUA).split(os.sep)[0] in VENDORED_DIRS:
            continue
        for fn in files:
            if not fn.endswith((".h", ".cpp")):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", errors="replace") as f:
                text = f.read()
            new = text
            for rx, rep in ns_decl_res:
                new = rx.sub(rep, new)
            if new != text:
                with open(p, "w", encoding="utf-8") as f:
                    f.write(new)
                n3 += 1
    print(f"native forward-decl namespace fixes: {n3} files")

    fixups()
    print("applied targeted fixups (services_factory.cpp)")


if __name__ == "__main__":
    main()
