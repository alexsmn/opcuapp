#!/bin/sh
# Re-vendors schema/ from OPCFoundation/UA-Nodeset at the given commit and
# rewrites schema/VERSION. See schema/README.md — a bump is a wire-visible
# change; review the generated-header diff before committing.
set -eu

if [ $# -ne 1 ]; then
  echo "usage: $0 <upstream-commit-sha>" >&2
  exit 2
fi

commit=$1
schema_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../schema" && pwd)
base="https://raw.githubusercontent.com/OPCFoundation/UA-Nodeset/${commit}/Schema"

for file in Opc.Ua.Types.bsd NodeIds.csv StatusCode.csv; do
  echo "fetching ${file}"
  curl -fsSL -o "${schema_dir}/${file}" "${base}/${file}"
done

commit_date=$(curl -fsSL \
  "https://api.github.com/repos/OPCFoundation/UA-Nodeset/commits/${commit}" |
  sed -n 's/.*"date": "\([0-9-]*\)T.*/\1/p' | head -1)

cat >"${schema_dir}/VERSION" <<EOF
release=1.05
repository=https://github.com/OPCFoundation/UA-Nodeset
branch=latest
commit=${commit}
commit_date=${commit_date}
fetched=$(date -u +%Y-%m-%d)
EOF

echo "updated ${schema_dir}/VERSION"
