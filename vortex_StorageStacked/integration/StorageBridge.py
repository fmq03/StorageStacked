# 引入 gem5 中承载 SystemC 模块的 SimObject 基类。
from m5.objects.SystemC import SystemC_ScModule
# 引入 TLM 目标 socket 类型，用于接收上游事务。
from m5.objects.Tlm import TlmTargetSocket
# 引入 SimObject 与 C++ 方法导出工具；本文件使用 PyBindMethod 导出 finish。
from m5.SimObject import SimObject, PyBindMethod
# 引入 gem5 的 Param 参数声明类型。
from m5.params import *
# 引入父对象参数代理 Parent，保留现有导入。
from m5.proxy import Parent


# 定义可由 Python 配置并在 C++ 中实例化的 SystemC 存储桥。
class StorageBridge(SystemC_ScModule):
    # 设置 gem5 SimObject 注册类型名。
    type = 'StorageBridge'
    # 指定对应的 C++ 模块完整类名。
    cxx_class = 'storage_axi::Bridge'
    # 指定生成参数和绑定代码时包含的 C++ 头文件。
    cxx_header = 'integration/storage.hh'
    # 将 C++ finish 方法导出给 Python，供成功结束时收尾。
    cxx_exports = [PyBindMethod('finish')]
    # 声明 64 位 TLM 目标 socket；宽度用于接口绑定，不限制单笔数据长度。
    tlm = TlmTargetSocket(64, 'Nonblocking transaction input')
    # 声明 AoU 资源平面数，默认 1；参数说明给出的范围为 1 到 4。
    planes = Param.Unsigned(1, 'AoU resource planes (1..4)')
    # 声明可复现物理 Flit 错误注入开关，默认关闭，用于测试重放。
    replay = Param.Bool(False, 'Inject reproducible 2% physical flit errors')
    # 声明在线内存模型制式，默认 hbm4。
    memsim_standard = Param.String('hbm4', 'Memory standard: hbm3, hbm4, lpddr5 or lpddr6')
    # 声明内存通道数，默认 2。
    memsim_channels = Param.Unsigned(2, 'HBM4 channel count for integration')
    # 声明原生内存时钟周期的整数缩放倍数，默认 1。
    memsim_scale = Param.Unsigned(1, 'Multiply native HBM clock period')
    # 声明内存入口、控制器与响应队列容量参数，默认 4。
    memsim_queue = Param.Unsigned(4, 'Native ingress/controller/response capacity')
    # 声明内存后端允许的在途 AXI burst 槽位数，默认 8。
    memsim_slots = Param.Unsigned(8, 'Bounded in-flight AXI bursts')
    # 声明压力测试用响应保持时长，以内存时钟 tick 计，默认不额外保持。
    memsim_response_hold = Param.Unsigned(0, 'Stress-test response hold in memory ticks')
    # 声明 AXI 时钟周期，默认 2 ns，由 gem5 转换为时间刻度。
    period = Param.Latency('2ns', 'AXI clock period')
    # 声明存储地址窗口基址，默认 0x90000000。
    base = Param.Addr(0x90000000, 'RAM base')
    # 声明存储地址窗口大小，默认 8192 字节。
    size = Param.UInt64(8192, 'Target memory window size in bytes')
    # 声明 TLM 主端在途事务槽位数，默认 4。
    outstanding = Param.Unsigned(4, 'Bounded TLM transaction slots')
    # 声明确定性通道延迟与响应反压开关，默认启用。
    stalls = Param.Bool(True, 'Deterministic stalls on all five channels')
    # 声明 CSV 日志和 VCD 波形的输出目录，默认空字符串。
    trace_dir = Param.String('', 'Directory for CSV/VCD traces')
