# 配置交接验收

## 2026-09-14 GitHub发布前复核

主仓库为https://github.com/fmq03/StorageStacked；新机器入口见[配置指引](setup.md)。
本次没有更新上游版本，也没有改变仿真模型。验收脚本统一关闭调试监听，避免交互终端
中误连接GDB导致仿真停止；补充递归子模块检查和本机HTTP报告查看方法。

| 验证范围 | 结果 |
|---|---|
| 干净源码恢复 | 11个上游库按固定提交恢复，适配安装执行两次，与当前源码一致 |
| 新工具目录配置 | 从157个原始缓存文件恢复；SS_OFFLINE=1 bootstrap_xpu通过，132个宿主工具包与8个私有运行库包匹配锁文件 |
| 新工具与新源码编译 | mem_sim重新构建、19项原生测试及在线C ABI校验通过 |
| 当前工作区交互终端回归 | CPU/定向请求7组、GPU/NPU四组、AoU/RAM兼容与反馈/观察器校验全部通过 |
| 源码入口检查 | Python/Shell/JSON检查通过；缺少子模块时提供递归初始化命令 |

本轮结果位于原维护机器的results/publish-20260914，含各套件summary.json、
source-restore.json、fresh-bootstrap.json、fresh-native-build.json和构建日志。
Git只分发源码、版本锁和指引，不分发这些结果或依赖压缩包。
新目录配置仍是在同一Linux宿主完成；新机器还需按指引执行构建和验收。
20260911依赖包仍可复用缓存，其system.bundle属于旧源码快照，优先从GitHub取得当前代码。

## 2026-09-11 交接基线

本轮已复核五个内部目录均属于主仓库，旧远端和本地集成提交仍是main祖先。
三个外部子模块及其八个递归依赖的固定提交未改变，git fsck连接性检查通过。
旧scripts/克隆工具、停用的内部Git元数据及旧CMake下载源码已归档到本地integrate_doc/handoff_20260911。
完全合并的临时迁移分支已删除；archive历史入口保留。已有AXI256、monorepo报告和原/mnt/d/storagestacked保留。

### 实际验证

| 验证范围 | 结果与证据 |
|---|---|
| 当前工作区完整构建 | bootstrap_xpu和build_xpu通过，最终构建日志为results/handoff-20260911/build-final-r2.log |
| CPU/定向请求到在线内存 | 19项原生测试、C ABI及7组链路通过；memsim-release/summary.json |
| GPU/NPU到在线内存 | 4组设备场景通过，检查真实计算、AXI/Flit/DRAM/DFI、HETTrace与完整波形；xpu-release/summary.json |
| 旧链路兼容 | 6组SimpleBurstMemory链路、5组RAM、CPU延迟反馈及观察器透明性通过；compat-release/summary.json |
| 从包恢复到新路径 | 11个外部Git依赖、132个宿主工具包、8个私有运行库包、Vortex工具链及3项CMake源码在新目录恢复成功 |
| 新环境禁止下载配置 | SS_OFFLINE=1 bootstrap_xpu通过；package-check-bootstrap.log和package-restore.json |
| 新源码/环境完整构建 | GPU/NPU与gem5重新构建通过；restore-build.log、restore-build-resume.log。途中只调整并行度并复用本轮已生成对象 |
| 新构建程序运行 | 19项原生测试、7组在线内存和CPU+GPU+NPU三源闭环通过；restored/summary.json |
| 包完整性检查 | 校验固定锁和逐文件SHA256；字节损坏、额外文件、错误锁版本均被拒绝；package-negative.json |

上表结果路径均相对于原维护机器的results/handoff-20260911，总报告为report.html，
总JSON为summary.json。这些完整结果不进入Git。

当前工作区在线内存套件有989笔父请求、1784个原生子请求；设备套件有29253笔父请求、29887个原生子请求。
同一路径中将内存时间尺度放大4倍，CPU基线完成增加16286ns，严格等于逐请求延迟差之和；
三源GPU从612增至1200周期，NPU从5068增至8359周期，CPU完成增加103882ns。
跨路径的新构建结果不作为性能优劣对照，验收依据是计算结果、真实响应及独立数据/时间检查。

### 交付内容与边界

配置入口见[配置指引](setup.md)，同时给出依赖包恢复和无包联网安装方法。
依赖包位于dist/storagestacked-deps-20260911.tar.gz，校验和在同名.sha256文件，均不提交Git。
包包含主仓库Git bundle、固定上游bare仓库、原始工具包和Bazel下载缓存；不包含已安装前缀或编译产物。
Ramulator的yaml-cpp/spdlog/argparse已按提交及SHA256锁定；CMake和SimX都使用该源码缓存，验收核对了实际编译依赖。

新目录验证仍在同一Linux宿主进行，不能替代第二台实体机器或其他系统版本的验收。
bootstrap阶段禁止下载已验证；Bazel构建允许联网，不承诺任意目标或整机完全离线重建。
HBM使用包含provisional时序的行为模型，GPU为SimX、NPU为RTL；当前没有通用functional/atomic、checkpoint或跨设备缓存一致性。
原始版本、实际工具和二进制哈希见各environment/manifest.json与xpu_manifest.json。
