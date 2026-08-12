#!/bin/bash
# deploy: verify the build locally, push main, wait for the pages workflow
# of this exact commit, then smoke-test the live site. the site itself is
# built and published by .github/workflows/deploy.yml from source — no build
# products in git. every step gates the next.
set -euo pipefail
cd "$(dirname "$0")"

# deploys ship commits; a dirty tree means what you tested isn't what ships
[ -z "$(git status --porcelain)" ] || { echo "working tree dirty — commit first" >&2; exit 1; }

# local build gate: never push a commit whose build is broken
bash build.sh
test "$(stat -f%z web/earth.wasm)" -gt 500000
test "$(stat -f%z web/earth.js)" -gt 50000

git push origin main
sha=$(git rev-parse HEAD)

# the workflow run for this sha can take a few seconds to register
for i in $(seq 1 12); do
	run_id=$(gh run list --workflow deploy.yml --commit "$sha" --json databaseId -q '.[0].databaseId' 2>/dev/null || true)
	[ -n "$run_id" ] && break
	sleep 5
done
[ -n "${run_id:-}" ] || { echo "no workflow run appeared for $sha" >&2; exit 1; }
gh run watch "$run_id" --exit-status

# live smoke: the served wasm should match the local build's byte count
# (warning only — the cdn can lag, and a CI toolchain drift also lands here)
live=$(curl -s "https://joshuahhh.com/soil/earth.wasm" -o /dev/null -w "%{size_download}")
local_size=$(stat -f%z web/earth.wasm)
if [ "$live" != "$local_size" ]; then
	echo "warning: live wasm is $live bytes (local $local_size) — cdn lag or toolchain drift" >&2
fi
echo "deployed $sha"
