#!/bin/bash
# make_livearea_mockups.sh <folder with outside.png and system.png>
# Puts the Vita's own LiveArea parts (cut from a screenshot) and the current
# LiveArea pictures into preview/livearea.template.html, making
# preview/livearea-mockups.html.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
parts="$1"
work="$(mktemp -d)"
uri() { printf 'data:image/png;base64,' > "$work/$2"; base64 -w0 "$1" >> "$work/$2"; }
uri "$parts/outside.png" OUTSIDE
uri "$parts/system.png" SYSTEM
uri "$here/../sce_sys/livearea/contents/bg.png" ROWSBG
uri "$here/../sce_sys/livearea/contents/startup.png" ROWSGATE
WORK="$work" perl -pe 'sub part { local $/; open my $f, "<", "$ENV{WORK}/$_[0]" or die; <$f> } s/\@\@(OUTSIDE|SYSTEM|ROWSBG|ROWSGATE)\@\@/part($1)/ge' \
	"$here/../preview/livearea.template.html" > "$here/../preview/livearea-mockups.html"
ls -la "$here/../preview/livearea-mockups.html"
