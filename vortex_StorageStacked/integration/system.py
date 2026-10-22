# 模块说明：把 gem5/Vortex 平台连接到独立的 AXI 存储服务。
"""Connect a gem5/Vortex platform to the independent AXI storage service."""
# 引入命令行解析器，读取运行配置文件路径。
import argparse
# 引入 JSON 工具，用于读取配置并保存退出状态。
import json
# 引入 Path，处理配置路径和输出目录。
from pathlib import Path
# 引入 gem5 运行时接口，用于实例化系统和推进仿真。
import m5
# 开始导入地址范围、gem5/TLM 转换桥、异构访存监控器及根对象。
from m5.objects import (AddrRange, Gem5ToTlmBridge64, HetAxiMonitor, Root,
                        # 继续导入存储桥和 SystemC 内核包装类型。
                        StorageBridge, SystemC_Kernel)
# 从运行时 soc 模块引入平台构造函数。
from soc import create


# 定义系统构造、地址映射、运行和结果记录的主流程。
def main():
    # 用模块文档字符串作为命令行帮助描述。
    parser=argparse.ArgumentParser(description=__doc__)
    # 要求调用方通过 --config 指定配置文件。
    parser.add_argument('--config',required=True)
    # 解析命令行，读取指定文件并将 JSON 转成配置字典。
    config=json.loads(Path(parser.parse_args().config).read_text())
    # 取得 gem5 输出目录，并转换为绝对路径。
    output=Path(m5.options.outdir).resolve()
    # 创建系统、用户态进程和已分配的地址区域表。
    system,process,regions=create(config,output)
    # 取得 architecture、AXI 和内存配置，便于后续传参。
    a=config['architecture'];axi=a['axi'];memory=a['memory']
    # 取出 Vortex BAR 对应的存储窗口起点与大小。
    base,size=regions['vortex_bar']
    # 开始构造 SystemC 存储桥 SimObject。
    system.axi=StorageBridge(
        # 传入窗口范围，并把 AXI 周期转换成带 ns 单位的字符串。
        base=base,size=size,period=f"{axi['period_ns']}ns",
        # 传入 TLM 在途事务上限和 AoU 资源平面数。
        outstanding=axi['outstanding'],planes=axi['planes'],
        # 传入确定性反压开关和物理 Flit 错误注入/重放测试开关。
        stalls=axi['stalls'],replay=axi['replay'],
        # 传入在线内存模型制式与通道数。
        memsim_standard=memory['standard'],memsim_channels=memory['channels'],
        # 传入内存时钟缩放倍数与队列容量。
        memsim_scale=memory['scale'],memsim_queue=memory['queue'],
        # 传入内存 burst 槽位数与响应保持时间。
        memsim_slots=memory['slots'],memsim_response_hold=memory['response_hold'],
        # 将 CSV/VCD 输出目录传给桥，结束存储桥参数列表。
        trace_dir=str(output))
    # 创建只响应 BAR 地址窗口的 gem5 到 64 位 TLM 转换桥。
    system.bridge=Gem5ToTlmBridge64(addr_ranges=[AddrRange(base,size=size)])
    # 将转换桥的 TLM 发起端连接到存储桥的目标端。
    system.bridge.tlm=system.axi.tlm
    # 创建主机与设备访存监控器。
    system.monitor=HetAxiMonitor(
        # 同时开启主机侧和设备侧访问记录。
        trace_host=True,trace_device=True,
        # 以 10^9/MHz 将主机频率换算为 fs 周期，采用整数除法。
        host_clock_period=10**9//a['host']['clock_mhz'],
        # 同样换算设备时钟周期，并完成监控器构造。
        device_clock_period=10**9//a['device_clock_mhz'])
    # 将监控器下游端口连接到 gem5/TLM 转换桥。
    system.monitor.mem_side_port=system.bridge.gem5
    # 将监控器上游端口接到系统内存总线的下游端口集合。
    system.monitor.cpu_side_port=system.membus.mem_side_ports
    # 创建用户态仿真的根对象，并让 SystemC 内核承载该系统。
    root=Root(full_system=False,systemc_kernel=SystemC_Kernel(system=system))
    # 实例化 gem5 对象及连接，在启动前完成系统初始化。
    m5.instantiate()
    # 分别处理 Vortex 命令处理器和 BAR 两个进程可见区域。
    for name in ('vortex_cp','vortex_bar'):
        # 从区域表取出当前区域的起始地址和字节长度。
        address,length=regions[name]
        # 建立虚拟地址到同值物理地址的映射，大小向上取整到 4 KiB，并禁止缓存。
        process.map(address,address,(length+4095)//4096*4096,cacheable=False)
    # 推进仿真，最多运行 architecture.max_ticks 指定的时间刻度。
    event=m5.simulate(a['max_ticks'])
    # 同时要求退出码为零且退出原因为最后一个活动线程结束，才判定成功。
    passed=event.getCode()==0 and 'exiting with last active thread context' in event.getCause()
    # 记录退出原因、退出码、当前 fs 时间刻度与成功标志。
    completion={'cause':event.getCause(),'code':event.getCode(),'tick_fs':m5.curTick(),'passed':passed}
    # 仅成功时调用 C++ 存储桥的 finish，导出最终统计和波形。
    if passed:system.axi.finish()
    # 将结束状态以缩进 JSON 保存到 completion.json，并追加换行。
    (output/'completion.json').write_text(json.dumps(completion,indent=2)+'\n')
    # 在标准输出打印退出原因、返回码和当前时间，供运行日志查看。
    print('EXIT:',event.getCause(),'code',event.getCode(),'tick',m5.curTick())
    # 未满足成功条件时抛出异常，使上层运行器识别失败。
    if not passed:raise RuntimeError('Device computation did not complete: '+str(completion))


# 当 gem5 以专用入口名加载此脚本时执行主流程。
if __name__=='__m5_main__':main()
