#!/bin/bash
# One-shot setup ON THE JETSON (JetPack 4.5.1 / L4T r32.5): install DeepStream 5.1
# from NVIDIA's apt repo, make sure nvcc exists, fetch the model, build the YOLO
# custom lib and the app. Needs ~2.5 GB free. Safe to re-run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUDO=$( [ "$(id -u)" = 0 ] && echo || echo sudo )

echo ">> apt: deepstream-5.1 + build deps"
$SUDO apt-get update
$SUDO apt-get install -y deepstream-5.1 cuda-nvcc-10-2 cuda-cudart-dev-10-2 \
	libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev build-essential git curl

# Some images (this one included) had CUDA binaries deleted by hand while the
# package stayed "installed"; a reinstall puts nvcc back.
if [ ! -x /usr/local/cuda-10.2/bin/nvcc ]; then
	echo ">> nvcc missing, reinstalling cuda-nvcc-10-2"
	$SUDO apt-get install -y --reinstall cuda-nvcc-10-2
fi
export PATH=/usr/local/cuda-10.2/bin:$PATH

echo ">> model"
"$ROOT/models/get-yolov4-tiny.sh"

echo ">> DeepStream-Yolo custom lib (nvcc, several minutes on a Nano)"
make -C "$ROOT" yolo-lib

echo ">> app"
make -C "$ROOT"

echo ">> done. Try:  $ROOT/scripts/run.sh --udp <your-pc-ip>:5000"
