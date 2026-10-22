# SDK 集成补丁

当前源码已包含这些修改；补丁记录集成差异，不需要重复应用。

- `0001-remove-unused-single-page-map.patch`：删除没有调用者、且忽略分配失败的旧单页映射入口。保留现有 `phy_to_virt_map()` 连续映射流程和对外 C API，解决 ARM 交叉编译的未初始化变量错误。
