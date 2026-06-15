#!/usr/bin/env bash
set -euo pipefail

PORT=${1:-8080}
QUIZ_FILE="quiz.html"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v python3 &>/dev/null; then
  echo "Error: python3 is required but not found." >&2
  exit 1
fi

if [[ ! -f "$SCRIPT_DIR/$QUIZ_FILE" ]]; then
  echo "Error: $QUIZ_FILE not found in $SCRIPT_DIR" >&2
  exit 1
fi

echo "Starting SRE Interview Quiz..."
echo ""
echo "  Open in browser: http://localhost:$PORT/$QUIZ_FILE"
echo ""
echo "Press Ctrl+C to stop."
echo ""

cd "$SCRIPT_DIR"
exec python3 -m http.server "$PORT" --bind 127.0.0.1 2>/dev/null
