#!/usr/bin/env python3
"""Namespace the vendored core/common sources by wrapping EVERY vendored file's
declarations in `namespace opcua`, so opcuapp's symbols are distinct from the
SCADA monorepo's and a Stage-C adapter translation unit can include both core
(`scada::`/global) and opcuapp (`opcua::...`) headers without ODR clashes.

Why a full wrap (rather than nesting only the obvious namespaces): the codebase
relies on a global `ToString` / `operator<<` overload set — each type header
contributes overloads (`ToString(Variant::Type)`, `ToString(AttributeId)`, ...)
and `debug_util` provides the generic `ToString<T>`. Namespacing only part of
that set shadows the rest for namespace-nested callers and breaks overload
resolution. Wrapping everything keeps the whole ecosystem together in `opcua`,
where it resolves consistently via enclosing-namespace lookup.

Per vendored file:
  - rewrite qualified references to ABSOLUTE form: `scada::` -> `opcua::scada::`,
    `base::` -> `opcua::base::` (works both inside the wrap and inside excluded
    global blocks such as `namespace std { hash<opcua::scada::NodeId> }`),
  - rewrite a few global-qualified `::Name` references that point at now-wrapped
    symbols back to relative form so enclosing lookup finds them,
  - wrap the post-preamble body in `namespace opcua { ... }`, EXCLUDING any
    `namespace std|boost|transport { ... }` block (closed/reopened around it)
    so std specializations, Boost.Log ADL helpers and external forward
    declarations stay at their original scope.

Native sources (root / binary / websocket) live in `namespace opcua` already
and resolve `scada::`/`base::` via enclosing lookup; they're left as-is except
that file-scope `namespace scada { ... }` forward-declaration blocks are
rewritten to `namespace opcua::scada`, and test files (anonymous-namespace TEST
bodies at file scope) get the same absolute reference rewrite.
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
OPCUA = os.path.join(os.path.dirname(HERE), "opcua")
VENDORED_DIRS = ("base", "scada", "metrics", "common", "net")

# boost_log.h is left at global scope (not wrapped): its macros reference
# `::`-qualified names and it is only ever included by opcuapp .cpp internals,
# never by a public header, so it cannot reach a Stage-C adapter TU.
NOWRAP = {"base/boost_log"}

ref_res = [
    (re.compile(r'(?<![\w:])scada::'), 'opcua::scada::'),
    (re.compile(r'(?<![\w:])base::'), 'opcua::base::'),
]
# Global-qualified references to symbols now wrapped into opcua. After the
# wrap, `::Name` (global) no longer resolves; rewrite to absolute `opcua::Name`
# (NOT relative `Name`, which could collide with a same-named member, e.g.
# variant.cpp's `::Format` disambiguating from a member `Format`).
MOVED_SYMS = ["Format", "StructWriter", "SharedValue", "DebugHolder", "UtfConvert"]
# Leading-`::` only: exclude an identifier / `>` / `)` / `]` / `:` / `.` before
# the `::` so a member access like `FormatHelperT<...>::Format` is NOT matched.
_LEAD = r'(?<![\w:>)\].])'
strip_global_res = [(re.compile(_LEAD + r':: *' + s + r'\b'), 'opcua::' + s)
                    for s in MOVED_SYMS]
strip_global_res.append((re.compile(_LEAD + r':: *operator<<'), 'opcua::operator<<'))
internal_ref_re = re.compile(_LEAD + r':: *internal::')

OPEN = "namespace opcua {\n"
CLOSE = "}  // namespace opcua (vendored)\n"
CLOSE_PLAIN = "}  // (re-open opcua after external block)\n"
preamble_re = re.compile(r'^﻿?\s*(#pragma|#include|//|/\*|\*/|\*|$)')
define_re = re.compile(r'^\s*#\s*(define|undef|error|warning|pragma)\b')
if_re = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b')
endif_re = re.compile(r'^\s*#\s*endif\b')
include_re = re.compile(r'^\s*#\s*include\b')
exclude_ns_re = re.compile(r'^\s*(inline\s+)?namespace\s+(std|boost|transport)\b')


def _is_pre(l):
    """A pre-declaration line: blank, comment, #pragma/#include, or #define."""
    return bool(preamble_re.match(l)) or bool(define_re.match(l))


def find_insertion(lines):
    """Index at which to open `namespace opcua` — after the leading
    preprocessor/include/comment block, but never inside a conditional block
    that only guards includes, and never before code guarded by an #if/#else.

    A leading `#if` block that contains an `#include` is treated as preamble:
    its includes stay outside the namespace, and the wrap opens before the
    first *code* line within the block (e.g. a whole-file `#ifndef _WIN32`
    guard), or after the block if it holds only includes (a conditional
    include). A code-only `#if`/`#else` block (e.g. debug_holder's NDEBUG
    selection) is left intact and the wrap opens before it.
    """
    i, n = 0, len(lines)
    while i < n:
        if _is_pre(lines[i]):
            i += 1
            continue
        if if_re.match(lines[i]):
            j, depth, end = i, 0, n
            has_include, first_code = False, None
            while j < n:
                if if_re.match(lines[j]):
                    depth += 1
                elif endif_re.match(lines[j]):
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
                if j > i:
                    if include_re.match(lines[j]):
                        has_include = True
                    elif (lines[j].strip() and not _is_pre(lines[j])
                          and not lines[j].lstrip().startswith('#')
                          and first_code is None):
                        first_code = j
                j += 1
            if has_include:
                if first_code is not None:
                    return first_code  # open inside the guard, after its includes
                i = end + 1            # include-only guard: skip past it
                continue
            return i                   # code-only #if/#else: open before it
        return i                       # plain code line
    return None

# Native-side rewrites.
ns_decl_res = [
    (re.compile(r'\bnamespace scada\b(?!::)'), 'namespace opcua::scada'),
    (re.compile(r'\bnamespace base\b(?!::)'), 'namespace opcua::base'),
    (re.compile(r'\bnamespace data_services\b(?!::)'), 'namespace opcua::data_services'),
]


def key_for(path):
    rel = os.path.relpath(path, OPCUA).replace(os.sep, "/")
    return rel[:-2] if rel.endswith(".h") else rel[:-4] if rel.endswith(".cpp") else rel


def _append_marker(out, marker):
    """Append a wrap marker, ensuring the previous line ends with a newline so a
    trailing `//` comment can't swallow the marker's brace."""
    if out and not out[-1].endswith("\n"):
        out[-1] += "\n"
    out.append(marker)


def wrap_excluding(lines, start):
    """Wrap lines[start:] in namespace opcua, closing/reopening around any
    top-level std/boost/transport namespace block."""
    out = lines[:start]
    _append_marker(out, OPEN)
    i, n = start, len(lines)
    while i < n:
        if exclude_ns_re.match(lines[i]):
            _append_marker(out, CLOSE_PLAIN)
            brace, started = 0, False
            while i < n:
                brace += lines[i].count('{') - lines[i].count('}')
                if '{' in lines[i]:
                    started = True
                out.append(lines[i])
                i += 1
                if started and brace == 0:
                    break
            _append_marker(out, OPEN)
        else:
            out.append(lines[i])
            i += 1
    _append_marker(out, CLOSE)
    return out


ns_decl_line_re = re.compile(r'^\s*(inline\s+)?namespace\b')


def transform_vendored(text):
    # Rewrite scada::/base:: to absolute opcua::... form, but NOT on namespace
    # declaration lines: `namespace scada::sub {` must stay as-is so the outer
    # wrap turns it into opcua::scada::sub (rewriting it would double the opcua
    # prefix -> opcua::opcua::scada::sub).
    out_lines = []
    for ln in text.splitlines(keepends=True):
        if not ns_decl_line_re.match(ln):
            for rx, rep in ref_res:
                ln = rx.sub(rep, ln)
        out_lines.append(ln)
    text = "".join(out_lines)
    text = internal_ref_re.sub('internal::', text)
    for rx, rep in strip_global_res:
        text = rx.sub(rep, text)
    lines = text.splitlines(keepends=True)
    start = find_insertion(lines)
    if start is None:
        return text
    out = wrap_excluding(lines, start)
    if out and not out[-1].endswith("\n"):
        out[-1] += "\n"
    return "".join(out)


def fixups():
    # debug_util's ToString<u16string> needs an operator<<(ostream, u16string);
    # in core that lives in boost_log.h (global). Now that boost_log is wrapped
    # into opcua too, provide the u16 operators right in debug_util (opcua) so
    # ordinary lookup at the template definition finds them.
    du = os.path.join(OPCUA, "base", "debug_util.h")
    if os.path.isfile(du):
        with open(du, encoding="utf-8", errors="replace") as f:
            t = f.read()
        if "operator<<(std::ostream& stream, const std::u16string&" not in t and "namespace opcua {" in t:
            inject = (
                "\ninline std::ostream& operator<<(std::ostream& stream,\n"
                "                                 const std::u16string& s) {\n"
                "  return stream << opcua::UtfConvert<char>(s);\n"
                "}\n"
                "inline std::ostream& operator<<(std::ostream& stream,\n"
                "                                 std::u16string_view s) {\n"
                "  return stream << opcua::UtfConvert<char>(s);\n"
                "}\n")
            t = t.replace(OPEN, OPEN + inject, 1)
            with open(du, "w", encoding="utf-8") as f:
                f.write(t)

    # boost_log.h stays global (NOWRAP) but calls the now-namespaced UtfConvert.
    blog = os.path.join(OPCUA, "base", "boost_log.h")
    if os.path.isfile(blog):
        with open(blog, encoding="utf-8", errors="replace") as f:
            t = f.read()
        t2 = re.sub(r'(?<![\w:])UtfConvert\b', 'opcua::UtfConvert', t)
        if t2 != t:
            with open(blog, "w", encoding="utf-8") as f:
                f.write(t2)

    # services_factory.cpp declares a helper in an anonymous namespace at file
    # scope (native, not wrapped), so its scada::/base::/Awaitable uses must be
    # fully qualified.
    sf = os.path.join(OPCUA, "services_factory.cpp")
    if os.path.isfile(sf):
        with open(sf, encoding="utf-8", errors="replace") as f:
            t = f.read()
        t = re.sub(r'(?<![\w:])scada::', 'opcua::scada::', t)
        t = re.sub(r'(?<![\w:])base::', 'opcua::base::', t)
        t = re.sub(r'(?<![\w:])Awaitable<', 'opcua::Awaitable<', t)
        with open(sf, "w", encoding="utf-8") as f:
            f.write(t)


def main():
    nv = 0
    for d in VENDORED_DIRS:
        root = os.path.join(OPCUA, d)
        if not os.path.isdir(root):
            continue
        for dirpath, _x, files in os.walk(root):
            for fn in files:
                if not fn.endswith((".h", ".cpp")):
                    continue
                p = os.path.join(dirpath, fn)
                if key_for(p) in NOWRAP:
                    continue
                with open(p, encoding="utf-8", errors="replace") as f:
                    text = f.read()
                new = transform_vendored(text)
                if new != text:
                    with open(p, "w", encoding="utf-8") as f:
                        f.write(new)
                    nv += 1
    print(f"vendored files wrapped in namespace opcua: {nv}")

    # Native files: forward-decl namespace fixes everywhere; full reference +
    # async-symbol qualification for test files (anon-namespace TEST bodies).
    async_test_syms = [
        "Awaitable", "AnyExecutor", "AnyExecutorFactory", "CoSpawn",
        "MakeAnyExecutor", "PostDelayedTask", "TestExecutor", "StartAwaitable",
        "WaitAwaitable", "WaitResult", "AwaitableResult",
    ]
    sym_res = [(re.compile(r'(?<![\w:.>])' + s + r'\b'), 'opcua::' + s)
               for s in async_test_syms]
    nn = 0
    for dirpath, _x, files in os.walk(OPCUA):
        if os.path.relpath(dirpath, OPCUA).split(os.sep)[0] in VENDORED_DIRS:
            continue
        is_test_dir = "/test/" in (os.path.relpath(dirpath, OPCUA).replace(os.sep, "/") + "/")
        for fn in files:
            if not fn.endswith((".h", ".cpp")):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", errors="replace") as f:
                text = f.read()
            new = text
            for rx, rep in ns_decl_res:
                new = rx.sub(rep, new)
            if fn.endswith("_unittest.cpp") or is_test_dir:
                for rx, rep in ref_res:
                    new = rx.sub(rep, new)
                for rx, rep in sym_res:
                    new = rx.sub(rep, new)
            if new != text:
                with open(p, "w", encoding="utf-8") as f:
                    f.write(new)
                nn += 1
    print(f"native files adjusted: {nn}")

    fixups()
    print("applied fixups (debug_util u16 operators, services_factory.cpp)")


if __name__ == "__main__":
    main()
