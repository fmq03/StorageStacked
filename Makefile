# 项目根目录入口：先完成所有接入前门禁，再进入 UCIe 端到端联调阶段。
# SystemC 安装参数会由子 make 自动继承；reference 必须先安装 integration 补丁。
.PHONY: preflight test-all golden-all wire-all boundary-all endpoint perf-all ucie-unit config-check reference-check
preflight test-all golden-all wire-all boundary-all endpoint perf-all ucie-unit config-check reference-check:
	$(MAKE) -C systemc $@
