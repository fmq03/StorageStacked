# 项目根目录入口：转发桥回归、链路组件测试和全链路测试。
# SystemC 安装参数会由子 make 自动继承；reference 必须先安装 integration 补丁。
.PHONY: preflight test-all golden-all wire-all boundary-all adapter perf-all ucie-unit config-check reference-check full-link full-link-all full-link-negative full-link-wave clean
preflight test-all golden-all wire-all boundary-all adapter perf-all ucie-unit config-check reference-check full-link full-link-all full-link-negative full-link-wave clean:
	$(MAKE) -C systemc $@
