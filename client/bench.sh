#!/bin/sh

# performance rig: measure how fast a saved view converges to its fully
# loaded state, as per-frame rmse against a reference screenshot.
#
#   ./bench.sh view.json [label]
#
# - press V in the client to save a view
# - the reference image is captured once per view; delete
#   bench/<view>.ref.ppm to recapture (e.g. after visual changes)
# - each run writes bench/<view>.<label>.csv and regenerates bench/<view>.png
#   overlaying every labeled run for that view, so label runs before/after a
#   change to compare them
# - runs are warm-cache by default (tile downloads hit ./cache); wipe that
#   dir for a cold-network run
# - don't resize the window between the capture and bench runs

view=$1
if [ -z "$view" ] || [ ! -f "$view" ]; then
	echo "usage: ./bench.sh view.json [label]" >&2
	exit 1
fi
# resolve the view path before cd-ing into the client dir
view="$(cd "$(dirname "$view")" && pwd)/$(basename "$view")"

cd "$(dirname "$0")" || exit 1

mkdir -p bench
base="bench/$(basename "$view" .json)"
ref="$base.ref.ppm"

if [ ! -f "$ref" ]; then
	echo "capturing reference for $view ..."
	./main --bench-capture "$view" "$ref" || exit 1
fi

label=${2:-$(date +%Y%m%d_%H%M%S)}
csv="$base.$label.csv"
./main --bench "$view" "$ref" "$csv" || exit 1

python3 bench_plot.py -o "$base.png" "$base".*.csv && echo "graph: $base.png"
