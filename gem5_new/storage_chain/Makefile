IVERILOG ?= iverilog
VVP      ?= vvp
BUILD    := build
SIMV     := $(BUILD)/axi_trace_boundary_simv

RTL := \
  rtl/xpu_axi_master.sv \
  rtl/axi_subset_checker.sv \
  rtl/axi_hettrace_monitor.sv \
  rtl/storage_chain_top.sv
TB := tb/tb_storage_chain.sv

.PHONY: all test lint clean

all: test

test: $(SIMV)
	$(VVP) $(SIMV)

lint:
	verilator --lint-only --timing -Wall -Wno-fatal \
		-Wno-BLKSEQ -Wno-UNUSEDSIGNAL --top-module tb_storage_chain \
		$(RTL) $(TB)

$(SIMV): $(RTL) $(TB) | $(BUILD)
	$(IVERILOG) -g2012 -Wall -Wimplicit -s tb_storage_chain -o $@ $(RTL) $(TB)

$(BUILD):
	mkdir -p $@

clean:
	rm -f $(SIMV)
	-@rmdir $(BUILD) 2>/dev/null || true
