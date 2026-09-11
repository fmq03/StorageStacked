# host 侧协同负载。
#
#   make            -> build/host_main
#   make clean
#
# -static 是刻意的：gem5 SE 模式能跑动态链接程序，但那要它去找 ld.so 和 libc，
# 版本一对不上就报一堆和本项目无关的错。静态链接把这类噪声全部去掉。
#
# -O2 没问题：所有共享内存与寄存器访问都是 volatile 的，编译器不会合并或删掉
# 它们。反过来说，往这个文件里加非 volatile 的共享区访问是危险的。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -static
BUILD   := build

all: $(BUILD)/host_main

$(BUILD)/host_main: host_main.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
