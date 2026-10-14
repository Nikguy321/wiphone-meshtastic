#!/usr/bin/env bash
# Usage: checkpoint.sh [loop-seconds]   -- snapshot progress, commit and push (optionally forever).
R=/home/user/wiphone-meshtastic
S=/tmp/claude-0/-home-user-wiphone-meshtastic/e73f209f-7803-522a-b473-d77076ab0347/scratchpad
W=/root/.claude/projects/-home-user-wiphone-meshtastic/e73f209f-7803-522a-b473-d77076ab0347/subagents/workflows
B=ccr-ea1d0e45-3qrky4
once() {
  (
    flock -w 120 9 || { echo "lock busy"; return; }
    python3 "$R/metroid-lorebook/tools/checkpoint.py" "$S" "$W" "$R/metroid-lorebook" || return
    cd "$R" || return
    git add metroid-lorebook >/dev/null 2>&1
    if git diff --cached --quiet -- metroid-lorebook; then echo "$(date -u +%H:%M) no changes"; return; fi
    git commit -q -m "metroid-lorebook: checkpoint research progress ($(date -u +%Y-%m-%dT%H:%MZ))

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_011xgWDvb5UahooVSPgyfRTD" -- metroid-lorebook
    for d in 0 2 4 8 16; do sleep $d; git push -q -u origin "$B" 2>&1 && { echo "$(date -u +%H:%M) pushed $(git rev-parse --short HEAD)"; return; }; done
    echo "$(date -u +%H:%M) PUSH FAILED"
  ) 9>/tmp/claude-0/metroid-checkpoint.lock
}
if [ -n "$1" ]; then while true; do once; sleep "$1"; done; else once; fi
