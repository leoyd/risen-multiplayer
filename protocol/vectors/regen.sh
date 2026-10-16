#!/usr/bin/env bash
# Régénère protocol/vectors/*.bin à partir des hexdumps de protocol/PROTOCOL.md (xxd -r),
# ou, avec --check, vérifie que chaque .bin redonne exactement son hexdump (xxd).
# Usage : protocol/vectors/regen.sh [--check]
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
doc=$here/../PROTOCOL.md
check=${1:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Extrait chaque bloc précédé de `<!-- vector: nom.bin -->` dans $tmp/nom.bin.txt.
awk -v out="$tmp" '
    /^<!-- vector: .* -->$/ { name = $3; next }
    name != "" && /^```/ { if (inside) { inside = 0; name = "" } else { inside = 1 } ; next }
    inside { print > (out "/" name ".txt") }
' "$doc"

shopt -s nullglob
status=0
count=0
for txt in "$tmp"/*.txt; do
    name=$(basename "$txt" .txt)
    count=$((count + 1))
    if [[ $check == --check ]]; then
        if [[ ! -f $here/$name ]]; then
            echo "[ÉCHEC] $name absent"; status=1
        elif diff -u "$txt" <(xxd "$here/$name") > /dev/null; then
            echo "[ OK ] $name"
        else
            echo "[ÉCHEC] $name diffère de PROTOCOL.md"; diff -u "$txt" <(xxd "$here/$name") || true; status=1
        fi
    else
        xxd -r "$txt" > "$here/$name"
        echo "$name ($(stat -c %s "$here/$name") octets)"
    fi
done
[[ $count -gt 0 ]] || { echo "aucun vecteur trouvé dans PROTOCOL.md"; exit 1; }
exit $status
