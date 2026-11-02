# 配置库接口

- `document.hpp`：读取文档、继承、选择与分层，保留来源路径/行号。
- `model.hpp`：模型 key 白名单、所有版本共用的联动推导、原子覆盖与 `build_model()`。
- `parse.hpp`：CLI 和库共用的严格数值、布尔解析。
- `fields.hpp`：普通字段的 typed setter/getter、Timing 别名和单位转换；
  用于解析、来源定位、最终配置导出和结果取值，不负责平台运行或参数联动。

本模块输出 `DramSpec`，不执行请求，不拥有 Controller 队列、PHY 或存储数据。
调用方仍需分别构造流量、MemorySystem、ControllerOptions 与 StorageModelOptions。
