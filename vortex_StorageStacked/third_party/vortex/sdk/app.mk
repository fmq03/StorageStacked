CONFIG ?= ../config.json
OUT ?= ../result/build
ifeq ($(strip $(HOST_SOURCES)),)
$(error Declare HOST_SOURCES, e.g. host.cpp)
endif
ifeq ($(strip $(KERNEL_SOURCES)),)
$(error Declare KERNEL_SOURCES, e.g. kernel.cpp)
endif
.PHONY: all
all:
	bash "$(SDK)/../../gem5/runtime/compile.sh" --config "$(CONFIG)" --output "$(OUT)" --model-header "$(MODEL_HEADER)" $(foreach src,$(HOST_SOURCES),--host-source "$(src)") $(KERNEL_SOURCES)
