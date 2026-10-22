# CONFIG and OUT are supplied by user/run.sh; paths are relative to this Makefile.
CONFIG ?= ../config.json
OUT ?= ../result/build
.PHONY: all
all:
	bash "$(SDK)/../runtime/compile.sh" --config "$(CONFIG)" --output "$(OUT)" --model-header "$(MODEL_HEADER)" $(SOURCES)
