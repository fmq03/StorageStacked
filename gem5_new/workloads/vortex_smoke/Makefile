# 裸机 rv32im flat image，给 Vortex 的 tap 做 smoke test 用。
#
# 用系统自带的 multilib riscv64-unknown-elf-gcc 打 rv32 目标，不需要 Vortex 的
# LLVM 工具链 —— 这个内核只用基础 rv32im 加一条手写编码的 custom0 指令。
#
# 产物是 .bin 而不是 .vxbin：vortex_gem5_load_kernel 的 flat-image 路径接受 .bin，
# 并固定装载到 0x80000000（设备库里写死的 startup_addr），所以链接地址必须一致。

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

ARCH    := -march=rv32im -mabi=ilp32
LOAD    := 0x80000000

BUILD   := build

all: $(BUILD)/kernel.bin

$(BUILD)/kernel.elf: kernel.S | $(BUILD)
	$(CC) $(ARCH) -nostdlib -nostartfiles -Wl,-Ttext=$(LOAD) -o $@ $<

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# 反汇编留着方便核对 custom0 那条指令的编码，以及确认没有编译器插进来的
# 非预期访存。
dis: $(BUILD)/kernel.elf
	$(OBJDUMP) -d $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean dis
