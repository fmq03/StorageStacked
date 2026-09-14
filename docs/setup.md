# 配置、交接与完整运行

本工程把CPU、Vortex GPU、CoralNPU的访存接入同一个AXI256/UCIe/在线mem_sim响应闭环。
五个内部模块已并入主仓库；只有gem5、coralnpu、vortex-gpu/vortex及Vortex递归依赖是子模块。
入口统一在根目录env/；protocol/是共享协议源码。
源码和依赖安装路径建议使用不含空格的Linux路径。

## 1. 主机条件

已验证平台：Ubuntu 20.04 / WSL2，Linux x86-64。宿主glibc需≥2.28。
建议至少16GiB内存、6个CPU线程、60GiB空闲磁盘；源码、编译缓存和完整波形会占用较多空间。
WSL建议在Linux文件系统中构建，例如~/StorageStacked。

宿主需有Git、Python3（≥3.8）、Bash、curl、tar/gzip/bzip2、make及基本构建工具。
Ubuntu可先由管理员安装：

```bash
sudo apt-get update
sudo apt-get install -y build-essential git python3 curl ca-certificates \
  tar gzip bzip2 unzip patch pkg-config autoconf automake libtool flex bison perl rsync
```

编译器、Python、SCons、CMake、Bazel、交叉工具链由下面的锁定环境准备。
无需物理GPU、商业仿真器或外部SystemC。仿真只使用gem5原生SystemC。

## 2. 没有依赖包：从Git与上游下载

主仓库为https://github.com/fmq03/StorageStacked。建议用Git克隆，保留子模块版本信息；
GitHub的源码ZIP不包含完整的上游子模块，不能直接代替下面的步骤。

```bash
git clone --recurse-submodules https://github.com/fmq03/StorageStacked.git StorageStacked
cd StorageStacked
# 已clone但未初始化子模块时执行：
git submodule sync --recursive
git submodule update --init --recursive

export SS_DEPS_ROOT="$HOME/.local/share/storagestacked-unified"
bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
```

若新电脑已配置GitHub SSH密钥，可将克隆地址换成git@github.com:fmq03/StorageStacked.git。
若仓库访问要求身份验证，使用有该仓库权限的GitHub账号。

正确命令是`git submodule update --init --recursive`，它取主仓库记录的确切提交。
不要使用`git submodule update --remote`或在各子模块里pull到最新分支；这会偏离已验证版本。
以后更新团队主仓库时，先`git pull --ff-only`，再执行上述submodule update。
五个内部目录无需分别clone或pull。

bootstrap_xpu包含基础环境配置；build_xpu包含mem_sim与gem5构建，无需再重复基础步骤。
bootstrap按锁文件下载已编译的GCC/G++、Python等工具包，不在本机从源码编译GCC，
也不替换系统/usr/bin/gcc。build会自动选择该环境中的编译器和库。
首次下载需要访问GitHub、conda-forge及Bazel依赖所用的上游站点。
全量构建需要数十分钟；gem5/mem_sim默认6个编译任务，可用AXI_JOBS调整，例如
`AXI_JOBS=12 bash env/build_xpu.sh`（本轮在32GiB内存机器上验证）。

## 3. 有依赖包：恢复源码与下载缓存

交付文件为`storagestacked-deps-20260911.tar.gz`和对应的`.sha256`。
依赖包不加入Git，解压后包含：

| 内容 | 用途 |
|---|---|
| system.bundle | 打包当时的主仓库源码与提交历史快照 |
| git/ | 三个外部库及八个递归依赖的独立bare仓库，保留固定提交和shallow边界 |
| cache/mamba/pkgs/ | 两份显式锁文件列出的原始Conda包 |
| cache/downloads/ | micromamba原始安装包 |
| cache/xpu-downloads/、cache/xpu-tools/ | Vortex原始工具链分片、LZ4、Ramulator的三项CMake源码依赖和固定Bazel可执行文件 |
| cache/bazel/cache/repos/ | Bazel下载缓存，不包含编译输出目录 |
| locks/、downloads.json、manifest.json | 版本锁、下载来源及逐文件SHA256清单 |

包中的上游源码是未打补丁的固定版本；系统内安装脚本会安装所需补丁和设备源码。
Ramulator的三项CMake依赖由bootstrap解压到SS_DEPS_ROOT/cmake-sources，构建直接使用
这些固定源码，避免CMake再次从Git下载。Ramulator作为Vortex内部依赖编译；整机内存后端使用mem_sim。
没有复制本机已安装的Conda前缀、修改过加载器的LLVM、NPU/gem5二进制、仿真结果或用户配置。

```bash
# 在收到的压缩包所在目录验证并解压：
sha256sum -c storagestacked-deps-20260911.tar.gz.sha256
tar -xzf storagestacked-deps-20260911.tar.gz

# 用解压后的真实绝对路径替换这里的路径。
export SS_BUNDLE_DIR=/data/storagestacked-deps-20260911
# 先取得当前主仓库；无需在线递归下载，下面的install会从包内恢复子模块。
git clone https://github.com/fmq03/StorageStacked.git StorageStacked
cd StorageStacked
export SS_DEPS_ROOT="$HOME/.local/share/storagestacked-unified"
python3 env/dependency_bundle.py install "$SS_BUNDLE_DIR" --deps-root "$SS_DEPS_ROOT"

# 从原始缓存重建到当前机器路径；此步骤禁止下载。
SS_OFFLINE=1 bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
```

若已取得匹配版本的主仓库，可跳过git clone，直接运行install。
20260911依赖包的system.bundle是旧源码快照，不包含之后的调试监听修复和文档更新；
上面的流程使用GitHub主仓库源码并复用旧包缓存。本次更新未改变四份依赖锁。
若只能通过bundle取得源码，可先执行git clone "$SS_BUNDLE_DIR/system.bundle" StorageStacked，
联网后再按下面的命令更新到主仓库当前版本。
install校验锁文件及包内容，从包内本地Git仓库初始化缺失的子模块，然后将origin恢复为
公开上游地址；已初始化的子模块只核对HEAD，不重置本地改动。
它只写下载缓存，不覆盖已经安装的工具环境。建议在新目录首次安装。

`SS_OFFLINE=1`约束的是bootstrap阶段。Bazel构建可能需要下载缓存尚未包含的依赖，
例如额外的Maven/JDK资源；依赖包可减少下载，但不承诺任意目标都能完全断网构建。
宿主apt软件包也未包含在包内。完整离线交付需另外验证目标机器的系统包和所选Bazel目标。

从bundle克隆后，主仓库origin指向本地bundle；联网后更新源码：

```bash
git remote set-url origin https://github.com/fmq03/StorageStacked.git
git pull --ff-only origin main
git submodule update --init --recursive
```

## 4. 跑完整流程

在主仓库根目录运行，每次使用尚不存在的结果目录：

```bash
bash env/run_memsim.sh results/acceptance-memsim
bash env/run_xpu.sh results/acceptance-xpu
# 旧测试内存/RAM、ID复用、CPU延迟反馈和观察器透明性：
bash env/run.sh results/acceptance-compat
```

- memsim：19项原生测试、C ABI检查、7组CPU/定向请求场景。
- xpu：CPU+NPU、CPU+GPU、三源、三源内存时间尺度×4，共4组。
- compat：SimpleBurstMemory链路、RAM和观察器对照。

三个结果目录的summary.json均应为passed=true；命令遇到失败会退出非零状态。
运行脚本自动激活工具环境，并关闭gem5调试监听。XPU入口依次打印“运行用例”、
“仿真结束，开始数据与链路校验”和“计算与链路校验通过”；仿真退出后仍需处理完整波形。
日志保留AXI五通道VCD、两端完整Flit、DRAM/DFI、HETTrace、数据与时间校验。
交接时复制整个用例目录及其_data目录、view_store.js，不单独复制HTML。
当前GPU是SimX，NPU是RTL；CPU程序/栈在本地主存，测试缓冲区走完整链路。
当前没有通用functional/atomic、checkpoint或跨设备缓存一致性支持。

### 查看报告

推荐在工程根目录启动仅供本机访问的HTTP服务，并保持该终端运行：

```bash
python3 -m http.server 8000 --bind 127.0.0.1 --directory results
```

然后用浏览器打开：

- CPU：http://localhost:8000/acceptance-memsim/cpu/memsim_view.html
- 三源：http://localhost:8000/acceptance-xpu/three/memsim_view.html
- AXI/Flit：http://localhost:8000/acceptance-xpu/three/trace_view.html

使用其他结果目录名时相应修改URL；结束服务按Ctrl+C。如果8000端口已占用，可改用8001。
直接双击HTML在部分环境可用，但Windows浏览器通过WSL文件路径打开时可能无法加载JS分块；
若看到“无法加载memsim_data/00000.js”，先确认同级数据目录存在，再改用上述HTTP方式。

### 运行停住时

先看对应结果目录的run.log。若最后出现remote gdb attached，说明仿真进入了调试接口。
本版所有验收入口显式传入--listener-mode=off，避免误连接或端口探测引起的停顿。
手动调用gem5时也将这个参数放在配置脚本路径之前。不要把7000调试端口当作报告页面端口。
--remote-gdb-port=0在当前Workload配置下不能代替--listener-mode=off。

## 5. 环境一致性与维护

- env/sources.lock.json固定三个外部源码版本；内部版本由主仓库提交决定。
- env/conda-linux-64.lock固定宿主工具环境，xpu-runtime-linux-64.lock固定私有运行库。
- env/xpu-artifacts.lock.json固定Bazel、LZ4、Vortex工具链及yaml-cpp/spdlog/argparse的URL与SHA256。
- 每个结果的environment/manifest.json记录源码、工具版本与二进制哈希；设备运行还记录xpu_manifest.json。
- 外部子模块在安装后出现modified属于预期，必要补丁保存在主仓库。不要用reset清掉适配。
- 新shell使用相同SS_DEPS_ROOT；需要手动命令时先source env/activate.sh。
- 改安装路径时从原始包重新bootstrap/build，不直接复制已安装前缀或Bazel编译缓存。
- 只有CPU时可以使用bootstrap.sh、build.sh、run_memsim.sh；共享环境仍使用同一份锁。

打包维护者在已准备好的环境中执行：

```bash
# 先提交需要交付的源码与指引，确保system.bundle包含这些提交。
python3 env/dependency_bundle.py pack dist/storagestacked-deps-20260911
tar -I 'gzip -1' -cf dist/storagestacked-deps-20260911.tar.gz \
  -C dist storagestacked-deps-20260911
(cd dist && sha256sum storagestacked-deps-20260911.tar.gz > storagestacked-deps-20260911.tar.gz.sha256)
```

pack读取Git提交和已下载的原始包，缺少必需工具包会报错；micromamba原始包缺失时按锁定版本下载。
dist/由.gitignore排除，不要强制加入Git。独立校验可运行：
`python3 env/dependency_bundle.py verify /path/to/extracted-package`。

本次交接的实际验证与产物位置见[交接验收记录](handoff-validation.md)。
