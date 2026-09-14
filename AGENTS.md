# StorageStacked 统一系统

接手先读 integrate_doc/HANDOFF.md 和 integrate_doc/09_migration.md（本地交接资料），
再读 README.md 和 docs/development.md。若交接资料未分发，以已提交文档为准。

- 主仓库为git@github.com:fmq03/StorageStacked.git；提交说明用中文，push须有用户明确授权。
- ucie-model、axi2flit、gem5_axi、mem_sim、gem5_new 是主仓库普通目录，直接维护源码。
  不重新建立内部 .git、gitlink 或构建时向内部模块应用补丁。
- gem5、coralnpu、vortex-gpu/vortex 是外部子模块，按 env/sources.lock.json 固定版本。
  外部必要改动在系统内保存补丁；不要擅自更新它们的上游版本或 reset 本地适配。
- CPU/GPU/NPU → gem5原生TLM → AXI256 → AXI2Flit → UCIe → 在线mem_sim，沿原链路返回。
  WDATA/RDATA=256bit，WSTRB=32bit，TLM Bridge64名称不表示AXI为64bit。
- 单进程、gem5主事件队列、gem5原生SystemC、统一1fs；不得链接第二套SystemC。
- 统一入口 env/bootstrap.sh、env/build.sh、env/run_memsim.sh；设备环境另见
  env/bootstrap_xpu.sh、env/build_xpu.sh、env/run_xpu.sh。完整链路显式选择backend aou及memory-backend memsim。
- gem5设备源码统一维护于gem5_new/gem5int/src/dev，构建自动刷新gem5中的副本。
  共享AoU帧格式在protocol/include。Vortex补丁只改外部SimX和ABI内部。
- 配置和依赖包说明见docs/setup.md，打包/恢复入口env/dependency_bundle.py。
  dist/依赖包不入Git；SS_OFFLINE=1仅约束bootstrap下载，Bazel构建的离线性需另行验证。
- 验收入口显式使用--listener-mode=off，避免交互终端中的GDB连接令仿真停住。
  HTML优先通过本机HTTP服务查看；直接打开WSL文件路径可能无法加载数据分块。
- 原目录 /mnt/d/storagestacked 禁止清理。integrate_doc、运行结果、构建产物不提交。
  已有AXI256结果及本次迁移备份保留；新验证使用独立结果目录。
- 仿真必须保留AXI五通道VCD、两端带时间戳的完整Flit日志及离线数据校验。
  HTML按需加载，交接复制整个用例目录及其_data目录、view_store.js。
- 当前GPU为Vortex SimX、NPU为CoralNPU RTL；CPU程序/栈在本地主存。
  尚无通用functional/atomic、checkpoint、跨设备缓存一致性、NPU重复启动支持。
