#!/bin/bash
# Fetch the Darknet YOLOv4-tiny COCO cfg + weights (AlexeyAB releases).
# The TensorRT engine is generated from these by nvinfer on first run.
set -euo pipefail
cd "$(dirname "$0")"
[ -f yolov4-tiny.cfg ]     || curl -fL -o yolov4-tiny.cfg     https://raw.githubusercontent.com/AlexeyAB/darknet/master/cfg/yolov4-tiny.cfg
[ -f yolov4-tiny.weights ] || curl -fL -o yolov4-tiny.weights https://github.com/AlexeyAB/darknet/releases/download/yolov4/yolov4-tiny.weights
ls -la yolov4-tiny.cfg yolov4-tiny.weights
