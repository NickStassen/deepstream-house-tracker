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
YOLO_DIR := third_party/DeepStream-Yolo
YOLO_REF ?= master
yolo-lib: $(YOLO_DIR)/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so

$(YOLO_DIR)/nvdsinfer_custom_impl_Yolo/Makefile:
	mkdir -p third_party
	git clone --depth 1 --branch $(YOLO_REF) https://github.com/marcoslucianops/DeepStream-Yolo.git $(YOLO_DIR)

$(YOLO_DIR)/nvdsinfer_custom_impl_Yolo/libnvdsinfer_custom_impl_Yolo.so: $(YOLO_DIR)/nvdsinfer_custom_impl_Yolo/Makefile
	CUDA_VER=$(CUDA_VER) $(MAKE) -C $(YOLO_DIR)/nvdsinfer_custom_impl_Yolo

clean:
	rm -rf build bin

.PHONY: all clean yolo-lib
