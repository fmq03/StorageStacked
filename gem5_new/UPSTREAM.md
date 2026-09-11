# 上游版本记录

本项目不直接修改上游基线，而是把自己的集成代码装进去（见
[docs/04-integration.md](docs/04-integration.md)）。代价是**上游漂移会让观测/异步补丁打不上**，
而且失效方式不总是明显的 —— 补丁能打上但语义变了，比补丁直接失败更难查。

可执行版本锁位于 [upstream.lock.json](upstream.lock.json)。下载脚本和预检共同读取它，
本文解释版本来源、访问条件和验证范围。新安装使用公开 gem5 基线；历史私有 fork 单独保留记录。

当前默认契约更新于 **2026-09-05**：CPU、Vortex 和 CoralNPU 的 memory-side 请求在 gem5
中的同一个 interconnect 观察点写成 HETTrace v2；gem5 只负责功能真值，不从外部存储模型
接收时序反馈。离线工具用 `convert --preset memsim` 把统一 AXI4 trace 投影为请求流，外部
`mem_sim` 的 `hbm_sim` 是存储控制器/DRAM 时序真值。

项目侧旧的在线 Ramulator、AXI/UCIe bridge 和 Python 内置 `memsim` 已退出默认架构。这里仍
钉住的 `third_party/ramulator` **只属于 Vortex SimX 自身的构建与运行时依赖**，不可删除，
也不可把它解释为本项目的离线或在线存储后端。

## 四棵主源码树

| 树 | 远端 | commit | 日期 | 最近的 tag |
|---|---|---|---|---|
| gem5 | `https://github.com/gem5/gem5.git` | `c8222cc67a399bfc01e8658dd14b30d5bfd634f9` | 2026-04-21 | `v25.1.0.1`（官方公开基线） |
| Vortex | `https://github.com/vortexgpgpu/vortex.git` | `d76b7f24e658867ab57e3942d7c648c3e6af072d` | 2026-07-29 | `v3.0` |
| CoralNPU | `https://github.com/google-coral/coralnpu.git` | `fcb74cfe79dbd184b9c53539490994e701981f80` | 2026-07-23 | `M3-2026-04-27` |
| mem_sim/hbm_sim | `https://github.com/GuXing25/mem_sim.git`（匿名不可访问，见离线方案） | `7945650579c44713ddec80acc2c82ae34e1f19a5` | 2026-09-03 | — |

Vortex SimX 的三个 submodule（不初始化的话 SimX 会以“缺头文件”的形式失败，看不出是
submodule 没拉）：

| submodule | commit |
|---|---|
| `third_party/softfloat` | `b51ef8f3201669b2288104c28546fc72532a1ea4` |
| `third_party/ramulator` | `e62c84a6f0e06566ba6e182d308434b4532068a5` |
| `third_party/cocogfx` | `b1befdb36df8af7ac9e2c96acaf81957aab5d107` |

表中的 Ramulator 只满足 Vortex SimX 的既有依赖；统一 trace 的下游时序实验使用上表单独
钉住的 `mem_sim/hbm_sim`。

## 2026-09-08：gem5 checkout 修正

原表把官方 remote 与 `2721ed751edac7d4cf3df574c6e0293343a14ba2` 私有 fork 提交配在一起，
导致官方 clone 后无法 checkout。核查时官方 commit API 对旧 commit 返回 422；
[官方 release](https://github.com/gem5/gem5/releases/tag/v25.1.0.1) 指向
[`c8222cc67a399bfc01e8658dd14b30d5bfd634f9`](https://github.com/gem5/gem5/commit/c8222cc67a399bfc01e8658dd14b30d5bfd634f9)。
Vortex、CoralNPU 两个固定 commit 的官方 API 均返回 200。

新目录可直接执行：

```bash
git clone --branch v25.1.0.1 --depth 1 https://github.com/gem5/gem5.git "$GEM5_HOME"
git -C "$GEM5_HOME" checkout --detach c8222cc67a399bfc01e8658dd14b30d5bfd634f9
```

已有官方 clone 缺少 tag 时先 `git fetch origin tag v25.1.0.1`。有本地修改时请用新目录，
避免直接切换正在使用的树。自动下载入口见[构建运行指南](docs/08-build-run.md)。

### 历史 fork 的适用范围

2026-09-05 报告使用的 gem5 是一棵**私有 fork**，HEAD 为
`2721ed751edac7d4cf3df574c6e0293343a14ba2`，在上游
`c8222cc67a`（`v25.1.0.1` hotfix）之上多了 3 个提交，来自一个与本项目无关的在先项目
（定制堆叠存储器仿真）。其中一个提交改过 `src/mem/comm_monitor.{cc,hh}` 和
`CommMonitor.py`，但统一 HETTrace 路径不使用该 fork 的 AXI direct socket，也不依赖旧的
per-device `CommMonitor` tap。项目自己的透明 AXI 监视器安装在共享 memory-side
interconnect 上；三源分类、请求/响应关联和 HETTrace v2 写出都由该监视器完成。

新安装不需要这 3 个提交；官方基线的构建、三源回归证据和复用依赖范围记录在
[验证报告](docs/05-validation-report.md)。预检对历史 fork 显示 `WARN` 并允许继续，
不会强迫现有用户覆盖工作树；其他未知提交仍会失败。

## mem_sim 无登录获取

2026-09-08 匿名核查：GitHub 仓库 API/网页返回 404，Git `info/refs` 返回 401。
这只能确认匿名不可访问，不能仅凭 404 判断仓库私有、迁移还是删除。更换 HTTPS/SSH
不会自动获得访问权；本项目也没有一个已经核实可用的公共替代地址。

交付方在拥有该源码的服务器上运行：

```bash
source scripts/native_env.sh
python3 scripts/upstreams.py package-memsim
```

输出 `build/deps/mem_sim-7945650579c44713ddec80acc2c82ae34e1f19a5.tar.gz`。
这是固定提交的 160 个已跟踪文件的构建源码快照，不携带 `.git`、登录配置或工作树未提交文件。
按交付内容要求排除上游 `堆叠存储模型交付手册.md`，由本项目更新后的构建/实验说明替代；
构建代码与许可证不变。排除清单也记录在 lock 中。
若输出已存在，脚本拒绝覆盖；重新打包时用 `--output` 指定新文件名。

将源码包作为项目的单独交付附件提供给接收者。接收者指定一个不存在的 `MEMSIM_HOME`：

```bash
python3 scripts/upstreams.py fetch --only memsim \
  --memsim-archive /path/to/mem_sim-7945650579c44713ddec80acc2c82ae34e1f19a5.tar.gz
python3 scripts/upstreams.py check --only memsim
```

导入检查 `git archive` 元数据中声明的 commit、路径和文件类型，再记录版本与文件清单；
预检接受这种无 `.git` 的源码树，并检查清单文件是否存在，不检测文件内容修改。
只导入可信交付方提供的源码包。构建产物可以添加在 `build/`。源码快照不是 Git 历史，
不能在其中 checkout。若有已授权的 Git 镜像，可用 `fetch --only memsim --memsim-url PATH_OR_URL`，
仍检出同一 commit；不要把 token 放进 URL。仓库权限变更与远端发布需由仓库所有者处理。

## 构建环境（本机实测通过的一组）

| | 版本 |
|---|---|
| OS | Ubuntu 24.04.4 LTS |
| gcc / g++ | 13.3.0 |
| Python（构建 gem5 用） | 3.12.3（系统 `/usr/bin/python3`，`gem5.opt` 链的是它的 `libpython`） |
| Python（跑本项目工具） | 3.8.20 也可 —— 工具只用标准库 |
| SCons | 在 `$GEM5_HOME/.venv/bin/scons`，**不在 PATH 上** |
| Bazel | CoralNPU 树内为 8.6.0（由 `.bazelversion` 钉住；树外 bazelisk 默认版本不作为构建口径） |
| CMake / generator | 3.28.3 / Unix Makefiles；Ninja 不是必需项，`hbm_sim` 要求 C++20 |
| Verilator | 系统命令 5.020（RTL 参考检查）；本轮 CoralNPU 的 Bazel 内部构建为 5.050 |
| Vortex LLVM 工具链 | `TOOLCHAIN_REV=v3.0`，`OSVERSION=ubuntu/focal`，装在 `$HOME/tools` |

## 怎么核对手上的树是不是这一组

```bash
for d in "$GEM5_HOME" "$VORTEX_HOME" "$CORALNPU_HOME" "$MEMSIM_HOME"; do
    printf '%-40s %s\n' "$d" "$(git -C "$d" rev-parse HEAD)"
done
git -C "$VORTEX_HOME" submodule status
```

上面的 Git 命令仅适用于 Git 源码树；包含离线快照时统一用
`python3 scripts/upstreams.py check`。对不上也不一定有问题，安装器会用
`patch -R --dry-run` 判断状态，但补丁成功本身不能替代功能回归。
真出问题时报错和重新生成补丁的做法见
[docs/04-integration.md](docs/04-integration.md#补丁打不上)。
