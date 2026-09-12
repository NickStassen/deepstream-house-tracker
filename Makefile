# deepstream-house-tracker — DeepStream 5.1 C application (Jetson, JetPack 4.5.1)
#
#   make            build bin/house-tracker
#   make yolo-lib   build the DeepStream-Yolo custom parser/engine lib
#   make clean

APP      := house-tracker
DS_ROOT  ?= /opt/nvidia/deepstream/deepstream-5.1
DS_INC   := $(DS_ROOT)/sources/includes
DS_LIB   := $(DS_ROOT)/lib
CUDA_VER ?= 10.2

CC       ?= gcc
CFLAGS   += -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS   += $(shell pkg-config --cflags gstreamer-1.0 gstreamer-video-1.0 glib-2.0)
CFLAGS   += -I$(DS_INC) -Isrc
LDFLAGS  += $(shell pkg-config --libs gstreamer-1.0 gstreamer-video-1.0 glib-2.0)
LDFLAGS  += -L$(DS_LIB) -lnvdsgst_meta -lnvds_meta -Wl,-rpath,$(DS_LIB) -lm

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

all: bin/$(APP)

bin/$(APP): $(OBJS)
	@mkdir -p bin
	$(CC) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# DeepStream-Yolo (marcoslucianops) provides the Darknet -> TensorRT engine
# builder and the YOLO bbox parser that nvinfer loads as a custom lib.
# Pinned to the last commit before its DeepStream 6.0 update: newer commits use
# the TensorRT 8 API (buildSerializedNetwork) and do not compile against the
# TensorRT 7.1 that ships with JetPack 4.5.1 / DeepStream 5.1.
YOLO_DIR := third_party/DeepStream-Yolo
# at the pinned commit the DeepStream implementation lives under native/
YOLO_IMPL := $(YOLO_DIR)/native/nvdsinfer_custom_impl_Yolo
YOLO_REF ?= 297e0e91195fe26512f0d4cb2b2fa563a3f14fd4
YOLO_URL := https://github.com/marcoslucianops/DeepStream-Yolo.git
yolo-lib: $(YOLO_IMPL)/libnvdsinfer_custom_impl_Yolo.so

$(YOLO_IMPL)/Makefile:
	mkdir -p $(YOLO_DIR)
	cd $(YOLO_DIR) && git init -q && git remote add origin $(YOLO_URL) && \
		git fetch -q --depth 1 origin $(YOLO_REF) && git checkout -q FETCH_HEAD

$(YOLO_IMPL)/libnvdsinfer_custom_impl_Yolo.so: $(YOLO_IMPL)/Makefile
	PATH=/usr/local/cuda-$(CUDA_VER)/bin:$$PATH CUDA_VER=$(CUDA_VER) $(MAKE) -C $(YOLO_IMPL)

clean:
	rm -rf build bin

.PHONY: all clean yolo-lib
