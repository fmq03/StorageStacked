# Docker 旧版本编译兼容修复

2026-10-10 的正式 `run.sh` 已处理旧 Docker 下的编译失败。
默认保留 Docker 的 seccomp 策略，在容器内追加兼容过滤器，让 `clone3`
返回 `ENOSYS`，使 glibc 回退到 `clone`。没有使用 `--privileged`，
也没有默认关闭 seccomp。镜像仍使用 `--pull=never --network=none`。

## 客户怎样使用

新下载：使用发布仓库最新版 `download.sh`，它会下载最新启动补丁，
`extract.sh` 解包后自动替换入口，并把冻结归档里的旧脚本保存为 `run-original.sh`。
手动下载全部分块时，还须把 Release 中的 `run.sh`、`docker-compat-entrypoint.py`、
`run-seccomp-check.sh`、`DOCKER_COMPATIBILITY.md` 与 `extract.sh` 放在同一目录。

已经解包：从 [发布仓库](https://github.com/hy2581/zhongxing-storagestacked-offline)
或 [Release](https://github.com/hy2581/zhongxing-storagestacked-offline/releases/tag/offline-20261009-burst)
取得这四个补丁文件，覆盖离线包目录中的对应文件。无需重新下载镜像。
例如在发布仓库目录内执行（目标按实际解压位置替换）：

```bash
cp run.sh docker-compat-entrypoint.py run-seccomp-check.sh DOCKER_COMPATIBILITY.md downloads/zhongxing-storagestacked-offline-20261009-burst/
cd downloads/zhongxing-storagestacked-offline-20261009-burst
sudo bash run.sh vortex smoke
```

等待最终 `PASS`，确认本次 `results/<运行编号>/summary.json` 的 `passed`
为 `true`。`[1/3]` 是编译，`[2/3]` 是仿真，`[3/3]` 是结果校验；
看到 `[2/3]` 只能说明编译已经通过。之后执行默认五项验收：

```bash
sudo bash run.sh
```

宿主机不需要 Python。兼容入口使用原镜像内已经具备的 Python，脚本只读挂载，
退出后不修改镜像。该入口也适用于 Docker 29.1.3。
若安装过滤器失败，会明确退出并报告原因，不会自动关闭安全限制。
使用容器内 `shell` 时也继承这项过滤器和 `no_new_privs`，无法通过后续
setuid 程序获取新的权限；镜像已有的 root 身份与编译、仿真不受影响。

## 原因与验证范围

客户原始错误：

```text
clang++: fatal error: unable to execute command: posix_spawn failed: Operation not permitted
```

后续 `make` 返回 2，外层 Python 报 `CalledProcessError`，都是编译失败的结果。
镜像中 Vortex LLVM 使用独立的 glibc 2.34；旧 Docker 的 seccomp 对
`clone3` 返回 `EPERM`，阻止了它回退。Docker 官方在
[20.10.10 发布记录](https://docs.docker.com/engine/release-notes/20.10/#201010)
中记录了 `clone3` 默认策略兼容支持，
[原始问题记录](https://github.com/moby/moby/issues/42680)解释了 glibc 的回退条件。
客户 `docker --version` 为客户端 20.10.0，服务端版本需用 `sudo docker version` 查看。

新增过滤器继续拒绝 `clone3`，只改变返回的错误码。
[Linux 内核 seccomp 文档](https://kernel.org/doc/html/latest/userspace-api/seccomp_filter.html)
说明多个过滤器叠加时仍执行既有拒绝规则，同为 `ERRNO` 时最近安装的过滤器提供错误码。
测试额外拒绝 `getpriority`，确认它在兼容入口下仍返回 `EPERM`。

制作方在 Docker 29.1.3 上使用同一冻结镜像，分别验证原入口复现相同编译错误、
修复入口通过编译、默认安全策略下修复入口通过编译，以及其他拒绝规则仍生效。
这是通过自定义策略模拟旧 Docker 的拒绝行为，没有在真实 Docker 20.10.0 服务端上运行。
正式启动脚本在该模拟策略下完成新的 Vortex SMOKE（`20261010-020026-64dca28f`），
41 → 42，计算、存储链路与波形校验均通过；默认策略下新的 CoralNPU SMOKE 与 AXI 也通过。证据见
[DOCKER_FIX_VALIDATION.json](https://github.com/hy2581/zhongxing-storagestacked-offline/blob/main/DOCKER_FIX_VALIDATION.json)。

此前同一镜像在 Docker 29.1.3 默认策略下的新运行
`20261010-010948-42c1906b` 已完成 AXI、Vortex SMOKE/LLM、CoralNPU SMOKE/LLM 五项，
两个 SMOKE 输出 `42`，两个 LLM 输出 `blu`，详见 `RUNTIME_RECHECK.json`。
本次补丁沿用已导入镜像，没有重新导入归档，也没有重跑 `test` 全回归。
客户此前关闭 seccomp 后已进入 `[2/3]`；客户完整验收报告及正式补丁的客户实测尚未提供。
原 412 个分块和 2026-10-09 镜像快照保留；本次更新发布的启动脚本、制作入口和文档。

## 查看源码和文档

```bash
sudo bash run.sh shell
```

进入容器后执行：

```bash
ls
cd vortex_StorageStacked
ls
ls docs
```

工作区包含 `axi_StorageStacked`、`vortex_StorageStacked` 和 `coralnpu_StorageStacked`。
输入 `exit` 返回宿主机。容器退出后删除，需保留的文件写入 `/results`。
失败编译日志位于宿主机 `results/<运行编号>/vortex/smoke/build.log`，例如：

```bash
sudo tail -n 120 results/20261010-005824-3914f7e2/vortex/smoke/build.log
```

## 临时诊断入口

此前的 `run-seccomp-check.sh` 仍保留，用于显式关闭本次容器的 seccomp 进行诊断：

```bash
sudo bash run-seccomp-check.sh vortex smoke
```

它现在调用统一的正式脚本，避免两套启动逻辑不一致。正常使用 `run.sh` 即可。
