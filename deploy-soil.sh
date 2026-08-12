#!/bin/bash
# build the web library and deploy web/ to the soil repo (github pages).
# every step gates the next: a failed build or copy must never push.
set -euo pipefail
cd "$(dirname "$0")"

SOIL=../soil
MSG="${1:-deploy}"

bash build.sh

# refuse to ship implausible artifacts (e.g. truncated by a full disk)
test "$(stat -f%z web/earth.wasm)" -gt 500000
test "$(stat -f%z web/earth.js)" -gt 50000

cp web/earth.js web/earth.wasm web/ar.html web/coi-serviceworker.js "$SOIL"/
cp web/index.html "$SOIL"/index.html

cd "$SOIL"
git add -A
if git diff --cached --quiet; then
	echo "nothing to deploy"
	exit 0
fi
git commit -m "$MSG"
git push

# wait for the pages build OF THIS COMMIT (latest may report the previous one)
sha=$(git rev-parse HEAD)
for i in $(seq 1 12); do
	read -r st built_sha <<< "$(gh api repos/joshuahhh/soil/pages/builds/latest -q '.status + " " + .commit')"
	[ "$st" = "built" ] && [ "$built_sha" = "$sha" ] && break
	sleep 15
done
[ "${built_sha:-}" = "$sha" ] || { echo "pages build did not complete for $sha" >&2; exit 1; }

# live smoke: the served wasm must match the local byte count
live=$(curl -s "https://joshuahhh.com/soil/earth.wasm" -o /dev/null -w "%{size_download}")
local_size=$(stat -f%z earth.wasm)
if [ "$live" != "$local_size" ]; then
	echo "warning: live wasm is $live bytes (local $local_size) — cdn may still be serving the old copy" >&2
fi
echo "deployed $sha (wasm $local_size bytes)"
