#!/bin/sh
# Render each docs/*.svg to a PNG next to it, at the SVG's own size and in the light scheme,
# with headless Chrome (for viewers that show no SVG).
# Usage: tools/svg2png.sh [file.svg ...]   (default: all of docs/*.svg)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
chrome=${CHROME:-"/c/Program Files/Google/Chrome/Application/chrome.exe"}
[ $# -gt 0 ] || set -- "$root"/docs/*.svg
for svg in "$@"; do
    svg=$(cd "$(dirname "$svg")" && pwd)/$(basename "$svg")      # Chrome needs an absolute URL
    size=$(head -c 400 "$svg" | sed -n 's/.*viewBox="0 0 \([0-9]*\) \([0-9]*\)".*/\1,\2/p')
    png=${svg%.svg}.png
    if command -v cygpath >/dev/null 2>&1; then
        url="file:///$(cygpath -m "$svg")"
        out=$(cygpath -w "$png")
    else
        url="file://$svg"
        out=$png
    fi
    "$chrome" --headless=new --disable-gpu --hide-scrollbars --blink-settings=preferredColorScheme=1 \
        --window-size="$size" --screenshot="$out" "$url" >/dev/null 2>&1
    echo "$png: $size"
done
