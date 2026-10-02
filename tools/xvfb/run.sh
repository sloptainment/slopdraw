#!/bin/bash
# tools/xvfb/run.sh OUT.png ["x1 y1 x2 y2" | "key:NAME" | title ...]  -- from the repo root, after a build: start Xvfb on a free display,
# launch build/mydraw, apply each drag (a click is a drag to the same point) key press or title print (xtitle.py, to stdout) in order, save a screenshot to OUT.png, stop the app and Xvfb.
set -e
S=$(dirname "$(readlink -f "$0")"); R=$(dirname "$(dirname "$S")"); out=$(readlink -f "$1"); shift
n=$(mktemp); Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$n" >/dev/null 2>&1 & xpid=$!
trap 'kill $pid $xpid 2>/dev/null; rm -f "$n"' EXIT
until [ -s "$n" ]; do sleep 0.1; done
export DISPLAY=:$(head -n1 "$n")
export XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$(mktemp -d)}  # QSettings: a throwaway config, never the real saved layout
"$R/build/mydraw" -geometry 1200x760+0+0 >/dev/null 2>&1 & pid=$!
sleep 2
for d in "$@"; do case $d in key:*) "$S/xkey.py" "${d#key:}";; title) "$S/xtitle.py";; *) "$S/xdrag.py" $d;; esac; sleep 0.5; done
scrot -o "$out"
