# 环境与复现

需要 Linux、CMake ≥3.20、C++17/20 编译器、SystemC 2.3.4、Verilator、
Python 3 + NumPy/Matplotlib、RISC-V bare-metal GCC、XeLaTeX/latexmk 和 IEEEtran/CTeX。
本机验证使用 /usr 的 SystemC；不得再链接其他版本。

```bash
bash env/bootstrap.sh  # 固定 VORTEX、SoftFloat、HardFloat；应用保存的外部补丁
bash env/build.sh      # 生成 VORTEX 配置、编译系统/RTL/内核
make acceptance       # 独立 RTL、内存与桥回归、在线闭环
python3 scripts/experiments.py  # 较长的论文对照实验
```

VORTEX 是外部固定版本依赖。补丁移除不用的外部宿主适配器，并把末级访存接到本系统。
不构建外部内存模拟器或其他处理器后端。上游的历史文档、变更记录不作为本系统功能说明。

DC 只读取本机库文件，库不入 Git：

```bash
python3 logic_die/syn/run.py --library /path/to/standard_cells.db --out results/dc-final
```

时钟默认 4 ns；修改 --clock 后，必须同步仿真时钟再重新测量，不能把不同时钟的结果混用。
每次运行保留独立 results 子目录；DC 的 RTL/库哈希记录在 inputs.json。
综合结果包含寄存器实现的权重存储，功耗报告是缺少活动文件的估计，不是测量值。
