#!/usr/bin/env python3
"""Rewrite include paths in the vendored tree so the vendored core/common
headers resolve under the `opcua/` prefix.

Only `#include "..."` lines are touched, and only when the path's first
segment is one of the vendored namespaces. `opcua/`, `transport/`, `net/`
and angle-bracket includes are left alone (except the transport backends —
see below).

  "scada/X"   -> "opcua/scada/X"
  "base/X"    -> "opcua/base/X"
  "metrics/X" -> "opcua/metrics/X"
  "common/X"  -> "opcua/common/X"

The native transport backends are relocated under opcua/transport/ during
vendoring, so their already-`opcua/`-prefixed self-references are remapped:

  "opcua/binary/X"    -> "opcua/transport/binary/X"
  "opcua/websocket/X" -> "opcua/transport/websocket/X"
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
OPCUA = os.path.join(os.path.dirname(HERE), "opcua")

PREFIXES = ("scada/", "base/", "metrics/", "common/", "net/")
# Transport backends relocated under opcua/transport/ during vendoring.
TRANSPORT_PREFIXES = ("opcua/binary/", "opcua/websocket/")
inc_re = re.compile(r'(#include\s+")([^"]+)(")')


def rewrite(line):
    m = inc_re.search(line)
    if not m:
        return line
    path = m.group(2)
    if path.startswith(PREFIXES):
        return line[:m.start(2)] + "opcua/" + path + line[m.end(2):]
    if path.startswith(TRANSPORT_PREFIXES):
        new = "opcua/transport/" + path[len("opcua/"):]
        return line[:m.start(2)] + new + line[m.end(2):]
    return line


def main():
    n_files = n_lines = 0
    for dirpath, _d, files in os.walk(OPCUA):
        for fn in files:
            if not fn.endswith((".h", ".cpp")):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            out = []
            changed = 0
            for ln in lines:
                nl = rewrite(ln)
                if nl != ln:
                    changed += 1
                out.append(nl)
            if changed:
                with open(p, "w", encoding="utf-8") as f:
                    f.writelines(out)
                n_files += 1
                n_lines += changed
    print(f"rewrote {n_lines} include lines across {n_files} files")


if __name__ == "__main__":
    main()
