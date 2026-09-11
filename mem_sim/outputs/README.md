# 仿真输出目录

项目内生成的仿真产物统一放在这里。推荐每次实验使用独立子目录，例如：

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --requests 32 \
  --cmd-trace outputs/exp01/commands.csv \
  --dfi-trace outputs/exp01/dfi.csv \
  --dump-timing-table outputs/exp01/timing.csv
```

CLI 会自动创建它所管理的输出父目录。保存 stdout 时，shell 会先于 CLI 打开
重定向文件，因此应先 `mkdir -p outputs/exp01`，再使用
`... | tee outputs/exp01/run_stats.txt`。统一清理全部产物并保留本说明：

```bash
make clean-outputs
```

`make delete` 是同义兼容命令。清理后不影响后续仿真，所需目录会自动重建。

本轮验证的结构化证据默认写入 `outputs/validation/`：

- `timing_boundaries.csv/json`
- `ramulator2_diff_v2.json`
- `performance_curves.csv/json`
- `sensitivity.csv`、`sensitivity_uncertainty.json`
- `dramsim3_aux.json`

`outputs/visualization/` 可保存离线可视化实验包：`commands.csv`、`dfi.csv`、
`stats.txt`、`thermal_map.txt`、`performance_curves.json` 和最终的
`dashboard.html`。HTML 自包含数据和 JavaScript，不依赖服务器或 Node.js。

这些文件是可再生成实验产物，不代替源码、配置和工具版本记录。
