# IEEE 双栏论文

采用 IEEEtran conference 文档类、中文正文和英文摘要。作者栏暂为匿名技术稿，
未虚构姓名、单位或发表状态。用 XeLaTeX 编译；PDF 见 main.pdf。

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r paper/requirements.txt
make -C paper
```

以上命令从仓库根目录执行。已有可用的 NumPy/Matplotlib 时可以省略虚拟环境步骤。
Makefile 优先使用仓库根目录的 `.venv/bin/python`，也可用 `PYTHON=/path/to/python` 指定。

- main.tex / references.bib：正文与引用。
- data/：经离线验证的测量摘要、输入哈希和综合摘要。
- collect_results.py：验证完整实验与当前源文件一致后，导出论文数据。
- make_figures.py：从数据生成矢量图和结果表。
- figures/logic_die_concept.png：使用内置“生成图片”工具制作的概念示意图。
  完整提示词在 figures/imagegen_prompt.txt；来源在 figures/provenance.json。
- 其他图：Python/Matplotlib 绘制；不把生成图中的视觉元素当作实验数据。
- 原始大日志、VCD、完整综合网表留在 results/，没有纳入 Git。

图表解释以正文及 data/README.md 为准。Software baseline 是单活跃 lane 的 VORTEX
标量算法实现；不能据此报告商用 GPU 或端到端 LLM 的加速倍数。
