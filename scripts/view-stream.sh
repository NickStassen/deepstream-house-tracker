#!/bin/bash
# On your PC: open the tracker's UDP MPEG-TS/H.264 stream with low latency.
#   scripts/view-stream.sh [port]   (default 5000)
PORT=${1:-5000}
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
exec ffplay -window_title "house-tracker :$PORT" -fflags nobuffer -flags low_delay -framedrop \
	-probesize 32 -analyzeduration 0 "udp://@:${PORT}?fifo_size=1000000"
