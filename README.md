# deepstream-house-tracker

A small, readable **C** DeepStream application for the Jetson Nano that detects and
tracks everyday household objects from a CSI camera and streams the annotated
video to your desk. It doubles as a template for future DeepStream projects:
swap the model config, keep the plumbing.

```
CSI camera (nvarguscamerasrc)  or  any URI (file / RTSP)
        │
   nvstreammux ─► nvinfer (YOLOv4-tiny, TensorRT FP16 engine) ─► nvtracker (NvDCF)
        │                                                              │
        └────────────────────────────── nvdsosd ◄──────────────────────┘
                                          │  pad probe → JSON events
                                         tee
                     ┌────────────────────┼─────────────────────┐
              H.264 → MPEG-TS → UDP   nveglglessink (local)   MP4 record
```

Why not a pile of homegrown "CV tool" repos? Because on Jetson the tooling already
exists and is hard to beat: GStreamer for plumbing, DeepStream for zero-copy
inference/tracking/OSD, TensorRT for the model. What was missing was a compact,
well-commented C example that wires them together for one camera and explains the
decisions. That is this repo.

## What it detects

COCO's 80 classes, which covers most of a house: person, cat, dog, chair, couch,
bed, dining table, TV, laptop, mouse, keyboard, remote, cell phone, book, clock,
vase, bottle, cup, bowl, fork, knife, spoon, potted plant, backpack, umbrella,
toothbrush, scissors, teddy bear, microwave, oven, toaster, sink, refrigerator...
(`configs/labels_coco.txt`). The model is Darknet **YOLOv4-tiny**, converted to a
**TensorRT FP16 engine** on the device the first time it runs (nvinfer + the
DeepStream-Yolo custom lib), then loaded from the `.engine` file every run after.

## Requirements

- Jetson Nano, JetPack **4.5.1** (L4T r32.5, CUDA 10.2, TensorRT 7.1) with
  **DeepStream 5.1** (installable from NVIDIA's apt repo, `scripts/setup-jetson.sh`
  does it).
- A working CSI camera. This was developed against an OV5647 using
  [jetson-nano-ov5647](https://github.com/NickStassen/jetson-nano-ov5647); an IMX219
  works out of the box.
- About 2.5 GB free disk for DeepStream + CUDA dev bits. DeepStream 5.1 is 1.6 GB
  installed; the setup script trims its sample videos/models afterwards.

Other JetPack / DeepStream versions: the app code is plain GStreamer + DeepStream
metadata API and should port with small changes (`DS_ROOT` in the Makefile, the
tracker lib names). The pinned DeepStream-Yolo commit is specific to TensorRT 7;
for TensorRT 8 use its current master instead.

## Setup (on the Nano)

```sh
git clone https://github.com/NickStassen/deepstream-house-tracker.git
cd deepstream-house-tracker
scripts/setup-jetson.sh      # apt install deepstream-5.1, repair CUDA, fetch model, build
```

`setup-jetson.sh` also audits the `cuda-*` packages for files deleted by hand
(this board had `nvcc`, the CUDA headers and `libcudart` removed to save space
while dpkg still listed them as installed) and reinstalls only what is broken.

Manual equivalent:

```sh
sudo apt-get install deepstream-5.1 cuda-nvcc-10-2 cuda-cudart-dev-10-2
models/get-yolov4-tiny.sh    # cfg + weights from AlexeyAB/darknet (24 MB)
make yolo-lib                # DeepStream-Yolo custom lib, nvcc, ~1 min
make                         # bin/house-tracker
```

## Run

```sh
# stream annotated 720p30 video to your PC, print events on stdout
scripts/run.sh --udp 192.168.1.219:5000

# on the PC
scripts/view-stream.sh 5000          # ffplay, low latency
```

The first start takes several minutes while TensorRT builds
`models/yolov4-tiny_b1_gpu0_fp16.engine`; later starts take a few seconds.

More options (`bin/house-tracker --help`):

| Flag | Meaning |
|---|---|
| `--source file:///clip.mp4` / `rtsp://...` | Use a URI instead of the CSI camera (hardware decode) |
| `--width/--height/--fps` | Camera capture mode (default 1280x720@30; 720p comes from the 2x2-binned sensor mode, best in low light) |
| `--tnr N`, `--tnr-strength F` | Argus temporal noise reduction (default 2 = high quality, strength 1.0). Big win on static indoor scenes |
| `--max-gain X` | Cap analog gain for auto exposure (default 8x). Trades brightness for noise in dim rooms |
| `--max-exposure-ms MS` | Cap exposure time (default: one frame period). Lower `--fps` to allow longer exposures |
| `--ee-mode N` | Edge enhancement (default 0 = off; sharpening amplifies sensor noise) |
| `--flip N` | Rotate/flip the camera (2 = 180 degrees) |
| `--tracker nvdcf\|klt\|iou` | Tracker backend (default NvDCF, visual features; KLT/IOU are cheaper) |
| `--tracker-width/-height` | Tracker working resolution, multiples of 32 |
| `--udp HOST:PORT`, `--bitrate KBPS` | H.264/MPEG-TS stream |
| `--display` | Also show on the Nano's own screen |
| `--record out.mp4` | Also write the annotated video |
| `--events events.jsonl` | Write events to a file instead of stdout |
| `--lost-frames N` | Frames without a match before a track is declared lost |
| `--summary-interval SEC` | Periodic count of what is in view, 0 = off |
| `-c CONFIG` | Different nvinfer config (another model) |

### Low light

Small CSI sensors get noisy fast as a room dims: auto exposure hits the frame
period and starts piling on analog gain. Defaults here are tuned for a house in
the evening: TNR on, gain capped at 8x, edge enhancement off. For a dark room
also drop the frame rate so exposure can lengthen:

```sh
scripts/run.sh --udp 192.168.1.219:5000 --fps 15            # 66 ms max exposure
scripts/run.sh --udp 192.168.1.219:5000 --fps 10 --max-gain 4
```

For daylight or fast motion, `--tnr 0 --max-gain 0` restores the sensor defaults.

## Events

JSON lines, one per line, easy to pipe into anything:

```json
{"event":"appeared","ts":"2026-09-11T20:31:07.412Z","frame":812,"id":14,"class":"cat","conf":0.71,"bbox":[402,318,187,142]}
{"event":"lost","ts":"2026-09-11T20:31:19.980Z","frame":1188,"id":14,"class":"cat","frames_tracked":361,"duration_s":12.53,"last_bbox":[955,301,171,139]}
{"event":"summary","ts":"2026-09-11T20:31:20.001Z","frame":1189,"frames_total":1189,"fps":21.4,"active":{"person":1,"chair":2,"laptop":1}}
```

`id` is the tracker's object id, stable while the object stays in view (and
across short occlusions, up to `maxShadowTrackingAge` frames in the NvDCF config).

## Layout

```
src/main.c        CLI, bus handling, lifecycle
src/pipeline.c    builds the GStreamer graph (sources, inference chain, output branches)
src/events.c      OSD pad probe: track bookkeeping, JSON events, FPS banner
configs/          nvinfer config, NvDCF tracker config, COCO labels
models/           fetch script; weights/cfg/engine live here (git-ignored)
scripts/          setup-jetson.sh, run.sh, view-stream.sh
third_party/      DeepStream-Yolo checkout (created by `make yolo-lib`, git-ignored)
```

## Adapting it to your next project

1. **Different model**: point `-c` at a new nvinfer config. Anything nvinfer
   supports works (Darknet via DeepStream-Yolo, ONNX, UFF, Caffe, TAO `.etlt`).
   Keep `network-mode=2` (FP16) on a Nano; INT8 is not supported on Maxwell.
2. **Different camera**: change `--width/--height/--fps`, or use `--source` for
   USB/RTSP sources through `uridecodebin`.
3. **Different reaction**: `events.c` is the only place that knows about tracks.
   Emit MQTT, hit a webhook, save a crop with `nvds_obj_enc_process` there.
4. **Second inference stage** (classifier on each detection): add another
   `nvinfer` after the tracker with `process-mode=2` and `operate-on-gie-id=1`.
5. **More cameras**: increase `batch-size` on nvstreammux and the nvinfer config,
   add more `sink_%u` pads. Expect the Nano to run out of steam quickly.

## Performance notes (Jetson Nano, MAXN)

Measured on this board with the OV5647 at 1280x720 (Argus scales from the
sensor's 1296x972 mode), `interval=1`, NvDCF at 480x288:

| Metric | Value |
|---|---|
| Pipeline frame rate | 30 fps (camera-bound) |
| GPU load (`tegrastats` GR3D) | 40 to 45 % |
| CPU load | 25 to 35 % per core |
| RAM used, whole system | 2.3 GB of 4 GB |
| First-run engine build | 2 min 44 s |
| Engine file | 31 MB, `configs/model_b1_gpu0_fp16.engine` |

YOLOv4-tiny FP16 alone runs roughly 20 to 25 fps on the Nano, so with
`interval=1` the detector sees every other frame and the tracker carries objects
in between. Set `interval=0` for detection on every frame if the latency of new
objects matters more than smoothness; expect the pipeline to drop to about
20 fps.

## License

MIT for the code in `src/`, `scripts/` and `configs/`. DeepStream-Yolo (fetched
into `third_party/`) is MIT; YOLOv4-tiny weights are from AlexeyAB/darknet.
