#!/usr/bin/env bash
# Capture the Accessibility nodes that a permission dialog adds.
#
# The interpreter recognises a permission by finding one container holding both
# an approve-ish and a deny-ish button. When a Claude Desktop release changes
# those labels, detection breaks silently — and a plain `dump` is useless for
# finding them, because the tree has hundreds of buttons and the dialog has to
# be on screen at the moment you look.
#
# This diffs the tree against itself: once with a prompt pending, once without.
# Whatever appears only in the "pending" capture is the dialog.
#
#   ./tools/capture-permission.sh
#
# Then copy the labels it prints into approveLabels / denyLabels in
# src/accessibility/selectors.js.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT="${TMPDIR:-/tmp}/claude-remote-capture"
mkdir -p "$OUT"

dump_buttons() {
  # stderr carries the log line; only the tree is wanted here.
  node bin/claude-remote.js dump --filter button 2>/dev/null
}

echo "1. Trigger a permission request in Claude Desktop and leave it on screen."
echo "   Do NOT answer it yet."
read -r -p "   Press Enter once the prompt is showing... " _
dump_buttons > "$OUT/pending.txt"
echo "   captured $(grep -c 'AXButton\|AXCheckBox\|AXRadioButton' "$OUT/pending.txt" || true) controls"

echo
echo "2. Now answer the prompt (deny is fine), so it leaves the screen."
read -r -p "   Press Enter once it is gone... " _
dump_buttons > "$OUT/gone.txt"
echo "   captured $(grep -c 'AXButton\|AXCheckBox\|AXRadioButton' "$OUT/gone.txt" || true) controls"

echo
echo "--- controls present only while the prompt was up ---"
# Lines starting with four spaces are the element descriptions; the path lines
# above them change constantly and only add noise.
# `diff` exits 1 when the files differ, and `set -o pipefail` propagates that,
# so a plain `|| echo` fired even on success. Capture first, then test.
ADDED=$(diff <(grep '^    ' "$OUT/gone.txt" | sort -u) \
             <(grep '^    ' "$OUT/pending.txt" | sort -u) \
        | grep '^>' | sed 's/^> */  /' || true)
if [ -n "$ADDED" ]; then
  printf '%s\n' "$ADDED"
else
  echo "  (nothing — was the prompt really on screen?)"
fi

echo
echo "Full captures: $OUT/pending.txt and $OUT/gone.txt"
