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

# Repair CUDA packages whose files were deleted by hand (this board had nvcc,
# the CUDA headers, lib64 symlink and libcudart/libcufft runtime libs removed
# while dpkg still listed the packages as installed). Docs, samples and Nsight
# are skipped on purpose: they are big and not needed to run DeepStream.
echo ">> auditing cuda-* packages for missing files"
REINST=""
for p in $(dpkg -l | awk '/^ii  cuda-/{print $2}'); do
	case "$p" in *documentation*|*samples*|*nsight*|*visual-tools*|*gdb*|*memcheck*|*nvprof*|*nvvp*|*nvdisasm*|*cupti*|*nvgraph*|*tools*|*libraries*|*toolkit*|*command-line*|*compiler*|*demo-suite*) continue;; esac
	n=$(dpkg -L "$p" 2>/dev/null | while read -r f; do [ -e "$f" ] || echo x; done | wc -l)
	[ "$n" -gt 0 ] && { echo "   $p: $n files missing"; REINST="$REINST $p"; }
done
if [ -n "$REINST" ]; then
	echo ">> reinstalling:$REINST"
	$SUDO apt-get install -y --reinstall $REINST
	$SUDO ldconfig
	rm -rf ~/.cache/gstreamer-1.0   # force a plugin rescan
fi
[ -e /usr/local/cuda-10.2/lib64 ] || $SUDO ln -s targets/aarch64-linux/lib /usr/local/cuda-10.2/lib64
export PATH=/usr/local/cuda-10.2/bin:$PATH

echo ">> model"
"$ROOT/models/get-yolov4-tiny.sh"

echo ">> DeepStream-Yolo custom lib (nvcc, several minutes on a Nano)"
make -C "$ROOT" yolo-lib

echo ">> app"
make -C "$ROOT"

echo ">> done. Try:  $ROOT/scripts/run.sh --udp <your-pc-ip>:5000"
