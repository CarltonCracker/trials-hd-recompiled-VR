#!/usr/bin/env bash
# Claim tasks from docs/vulkan-perf/PLAN.md. Claims live in the git common dir
# (shared by every worktree of this repo, never committed), and a claim is an
# atomic mkdir, so two agents can't take the same task.
#
#   tools/vk_claim.sh list                      all claims and their status
#   tools/vk_claim.sh claim  <TASK> <agent>     take a task (fails if taken)
#   tools/vk_claim.sh status <TASK> <text>      progress note, e.g. "measuring", "blocked: ..."
#   tools/vk_claim.sh done   <TASK> <text>      finished; text = measured result
#   tools/vk_claim.sh release <TASK>            give the task back (abandoned)
set -euo pipefail
dir="$(git rev-parse --path-format=absolute --git-common-dir)/vk-perf-claims"
mkdir -p "$dir"
cmd="${1:-list}"
now() { date '+%Y-%m-%d %H:%M'; }
case "$cmd" in
  list)
    printf '%-6s %-10s %-18s %-17s %s\n' TASK STATE OWNER UPDATED NOTE
    for d in "$dir"/*/; do
      [[ -d "$d" ]] || continue
      t="$(basename "$d")"
      printf '%-6s %-10s %-18s %-17s %s\n' "$t" "$(cat "$d/state" 2>/dev/null)" \
        "$(cat "$d/owner" 2>/dev/null)" "$(cat "$d/updated" 2>/dev/null)" "$(cat "$d/note" 2>/dev/null)"
    done ;;
  claim)
    t="$2"; who="$3"
    if mkdir "$dir/$t" 2>/dev/null; then
      echo "$who" >"$dir/$t/owner"; echo claimed >"$dir/$t/state"; now >"$dir/$t/updated"
      echo "claimed $t for $who"
    else
      echo "$t is already taken: $(cat "$dir/$t/owner" 2>/dev/null) ($(cat "$dir/$t/state" 2>/dev/null))" >&2
      exit 1
    fi ;;
  status|done)
    t="$2"; shift 2
    [[ -d "$dir/$t" ]] || { echo "$t is not claimed" >&2; exit 1; }
    [[ "$cmd" == done ]] && echo done >"$dir/$t/state" || echo active >"$dir/$t/state"
    echo "$*" >"$dir/$t/note"; now >"$dir/$t/updated" ;;
  release)
    rm -rf "${dir:?}/$2"; echo "released $2" ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac
