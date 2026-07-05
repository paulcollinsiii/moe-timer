#!/usr/bin/env bash
# Validates that the commit subject line follows Conventional Commits format.
# Invoked by pre-commit as a commit-msg hook; $1 is the commit message file.
#
# Valid format: <type>[(<scope>)][!]: <description>
# Valid types:  feat fix docs refactor test chore perf style ci build revert
#
# Examples:
#   feat(timer): add RUNNING/PAUSED state machine
#   fix: correct deep-sleep GPIO wakeup mask
#   chore!: upgrade ESP-IDF to v5.3 (breaking change)

set -euo pipefail

COMMIT_MSG=$(head -n 1 "$1")

# Skip auto-generated git messages (merge, revert, fixup, squash)
if echo "$COMMIT_MSG" | grep -qE "^(Merge|Revert|fixup!|squash!)"; then
    exit 0
fi

TYPES="feat|fix|docs|refactor|test|chore|perf|style|ci|build|revert"
PATTERN="^(${TYPES})(\([a-zA-Z0-9_/-]+\))?!?: .+"

if ! echo "$COMMIT_MSG" | grep -qE "$PATTERN"; then
    echo ""
    echo "ERROR: Commit message must follow Conventional Commits format."
    echo "  Format: <type>[(<scope>)][!]: <description>"
    echo "  Types:  feat fix docs refactor test chore perf style ci build revert"
    echo "  Got:    $COMMIT_MSG"
    echo ""
    echo "  Examples:"
    echo "    feat(timer): add RUNNING/PAUSED state machine"
    echo "    fix: correct deep-sleep GPIO wakeup mask"
    echo "    test(schedule): add day-type boundary conditions"
    echo ""
    exit 1
fi
