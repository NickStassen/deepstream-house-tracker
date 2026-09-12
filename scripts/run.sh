#!/bin/bash
# Run the tracker from the repo root with sane defaults. Extra args pass through.
#   scripts/run.sh --udp 192.168.1.219:5000
#   scripts/run.sh --source file:///path/clip.mp4 --record out.mp4 --events events.jsonl
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH=/opt/nvidia/deepstream/deepstream-5.1/lib:${LD_LIBRARY_PATH:-}
cd "$ROOT"
exec bin/house-tracker "$@"
