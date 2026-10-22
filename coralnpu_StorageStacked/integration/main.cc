// 引入顶层仿真模块 Demo；其头文件间接提供设备接口、接入参数和 SystemC 声明。
#include "storage.hh"
// 引入公共存储的 UCIe 编译参数，用于核对运行配置与当前构建是否匹配。
#include "link_config.h"
// 引入 Boost 属性树，用来保存解析后的 JSON 配置。
#include <boost/property_tree/ptree.hpp>
// 引入 Boost JSON 解析器，支持从文件或字符串流读取 JSON。
#include <boost/property_tree/json_parser.hpp>
// 引入文件输出流，用于保存配置快照和仿真结束状态。
#include <fstream>
// 引入标准输出与标准错误流，用于打印用法、检查信息和错误原因。
#include <iostream>
// 引入输出格式控制工具；后面用 setprecision 设置浮点数的有效数字位数。
#include <iomanip>
// 引入字符串流，用于将设备库返回的 JSON 文本交给解析器。
#include <sstream>
// 使用 SystemC 核心命名空间，后续可直接调用 sc_start、sc_time_stamp 等接口。
using namespace sc_core;
// SystemC 程序入口：argc 为命令行参数数量，argv 保存程序名及各参数。
int sc_main(int argc,char** argv) {
    // 仅传入 --check 时打印运行器说明并返回 0；此分支不加载配置，也不执行仿真。
    if(argc==2 && std::string(argv[1])=="--check") {std::cout<<"CoralNPU native / standalone SystemC / online mem_sim\n";return 0;}
    // 正常运行需要配置文件和输出目录两个参数；数量不符时打印用法并返回 2。
    if(argc!=3) {std::cerr<<"usage: coralnpu_sim resolved.json output-directory\n";return 2;}
    // 保存输出目录路径；目录须由调用方提前创建，本文件不负责创建目录。
    const std::string directory=argv[2];
    // 进入异常保护范围；JSON 读取、配置核对及仿真中的标准异常由后面的 catch 处理。
    try {
        // 将全局仿真时间分辨率设为 1 fs，后续时间刻度值均以飞秒计量。
        sc_set_time_resolution(1,SC_FS);
        // 创建配置属性树 c，并从第一个命令行参数指定的 JSON 文件中读取配置。
        boost::property_tree::ptree c;boost::property_tree::read_json(argv[1],c);
        // 引用 architecture 配置子树，并创建继承公共存储默认参数的接入参数对象 p。
        auto& a=c.get_child("architecture");storage_axi::ConfigParams p;
        // 设置日志输出目录，并读取设备程序 ELF 路径 kernel；相对路径以进程工作目录为基准。
        p.trace_dir=directory;p.kernel=c.get<std::string>("kernel");
        // 读取 AXI 周期并从 ns 换算为 fs：1 ns = 1000000 fs。
        p.period=a.get<uint64_t>("axi.period_ns")*1000000;
        // 将设备频率 MHz 换算为周期 fs；使用整数除法，例如 500 MHz 对应 2000000 fs。
        p.device_period=1000000000/a.get<unsigned>("device_clock_mhz");
        // 读取仿真时间上限 max_ticks（fs）和设备适配器允许的最大在途请求数。
        p.max_ticks=a.get<uint64_t>("max_ticks");p.outstanding=a.get<unsigned>("axi.outstanding");
        // 读取每笔 AXI burst 的拍数；该字段缺省时使用 4 拍，合法性由 Master 构造函数检查。
        p.burst_beats=a.get<unsigned>("axi.burst_beats",4);
        // 读取存储链路平面数、适配器反压开关和链路重放开关。
        p.planes=a.get<unsigned>("axi.planes");p.stalls=a.get<bool>("axi.stalls");p.replay=a.get<bool>("axi.replay");
        // 读取在线 MEMSIM 的通道数和时序缩放参数。
        p.memsim_channels=a.get<unsigned>("memory.channels");p.memsim_scale=a.get<unsigned>("memory.scale");
        // 读取在线 MEMSIM 的槽位数和队列深度。
        p.memsim_slots=a.get<unsigned>("memory.slots");p.memsim_queue=a.get<unsigned>("memory.queue");
        // 读取在线 MEMSIM 的响应保持参数和内存制式名称。
        p.memsim_response_hold=a.get<unsigned>("memory.response_hold");p.memsim_standard=a.get<std::string>("memory.standard");
        // 开始检查 UCIe 编译参数；配置要求的 lane 数必须与当前运行器的编译值相同。
        if (a.get<unsigned>("ucie.lanes") != LINK_LANES ||
            // 继续核对 UCIe 速率，单位为 GT/s；任一不匹配都会使整个条件成立。
            a.get<double>("ucie.rate_gtps") != LINK_RATE_GTPS ||
            // 继续核对每个符号传输的位数。
            a.get<unsigned>("ucie.bits_per_symbol") != LINK_BITS_PER_SYMBOL ||
            // 继续核对周转时间，单位为 ns；这里对两个浮点配置值采用直接比较。
            a.get<double>("ucie.tat_ns") != LINK_TAT_NS)
            // UCIe 参数不匹配时抛出异常，防止使用不符合本次配置的运行器继续仿真。
            throw std::invalid_argument("UCIe build differs from selected project configuration");
        // 创建设备模型配置属性树，保存实际加载的设备库报告的参数。
        boost::property_tree::ptree native;
        // 调用设备库接口获取模型配置 JSON 文本，并包装成可读取的字符串流。
        std::istringstream nativeText(coralnpu_configuration());
        // 将设备库报告的 JSON 解析到 native 属性树中。
        boost::property_tree::read_json(nativeText,native);
        // 依次检查指令紧耦合存储器 ITCM 和数据紧耦合存储器 DTCM 的容量，单位为 KiB。
        for (auto key : {"itcm_kib","dtcm_kib"})
            // 将实际模型容量与 architecture.npu 中同名配置比较。
            if(native.get<unsigned>(key)!=a.get<unsigned>(std::string("npu.")+key))
                // 任一 TCM 容量不匹配时抛出异常，避免用错误容量的 RTL 模型运行程序。
                throw std::invalid_argument("Loaded RTL TCM geometry differs from configuration");
        // 检查实际模型的取指 L0 开关是否与配置一致。
        if(native.get<bool>("fetch_l0_enabled")!=a.get<bool>("npu.fetch_l0_enabled") ||
           // 同时要求实际模型启用了 RVV 和浮点功能；任一功能缺失都会判定不匹配。
           !native.get<bool>("rvv_enabled") || !native.get<bool>("float_enabled"))
            // 取指配置或必需功能不符合要求时抛出异常。
            throw std::invalid_argument("Loaded RTL features differ from configuration");
        // 保存设备库实际报告的模型配置；临时文件流在本语句结束时销毁并关闭文件。
        std::ofstream(directory+"/npu_model.json")<<coralnpu_configuration()<<"\n";
        // 打开 UCIe 配置快照文件，记录当前运行器实际编译使用的链路参数。
        std::ofstream link(directory+"/ucie_config.json");
        // 以 17 位有效数字输出链路参数，先写入 lane 数和速率，减少浮点序列化的精度损失。
        link<<std::setprecision(17)<<"{\"lanes\":"<<LINK_LANES<<",\"rate_gtps\":"<<LINK_RATE_GTPS
            // 继续写入每符号位数和周转时间，并结束 JSON 对象。
            <<",\"bits_per_symbol\":"<<LINK_BITS_PER_SYMBOL<<",\"tat_ns\":"<<LINK_TAT_NS<<"}\n";
        // 显式关闭 UCIe 配置文件。
        link.close();
        // 下面记录 C++ 可执行程序实际读取的参数，便于与 Python 生成的 resolved.json 对照。
        // Record values read by the executable, independent of Python's resolved snapshot.
        // 打开实际运行参数快照文件；该文件记录运行参数的一个子集。
        std::ofstream config(directory+"/config.json");
        // 写入运行时类型、设备名称以及设备时钟周期，周期单位为 fs。
        config<<"{\"runtime\":\"standalone-systemc\",\"device\":\"coralnpu\",\"device_period_fs\":"<<p.device_period
              // 写入继承的外部存储地址窗口起点、大小以及 AXI 时钟周期，周期单位为 fs。
              <<",\"axi\":{\"base\":"<<p.base<<",\"size\":"<<p.size<<",\"period\":"<<p.period
              // 继续写入最大在途请求数、burst 拍数、平面数和 JSON 布尔类型的重放开关。
              <<",\"outstanding\":"<<p.outstanding<<",\"burst_beats\":"<<p.burst_beats<<",\"planes\":"<<p.planes<<",\"replay\":"<<(p.replay?"true":"false")
              // 继续写入反压开关、MEMSIM 槽位数，结束 JSON 并关闭文件。
              <<",\"stalls\":"<<(p.stalls?"true":"false")<<",\"memsim_slots\":"<<p.memsim_slots<<"}}\n";config.close();
        // 创建名为 storage 的顶层模块，内部连接设备主端、公共存储、时钟复位和协议监控，并开启波形记录。
        storage_axi::Demo demo("storage",p);
        // 只要设备尚未结束且当前仿真时间未达到上限，就继续推进仿真。
        while(!demo.done() && sc_time_stamp().value()<p.max_ticks)
            // 每次最多推进 100000000 fs（100 ns）；临近上限时只推进剩余时间，模块内部事件仍由 SystemC 调度。
            sc_start(sc_time::from_value(std::min<uint64_t>(100000000,p.max_ticks-sc_time_stamp().value())));
        // 仅当设备已结束且 mailbox 为约定成功标志 0x600d0000 时，才判定本入口执行成功；后续仍需独立验收。
        bool passed=demo.done() && demo.mailbox()==0x600d0000;
        // 打开仿真结束状态文件，成功、错误 mailbox 或超时都会在此记录结果。
        std::ofstream result(directory+"/completion.json");
        // 写入 passed 布尔值与结果码；成功为 0，失败为 1。
        result<<"{\"passed\":"<<(passed?"true":"false")<<",\"code\":"<<(passed?0:1)
              // 根据设备是否结束选择原因文本，并准备写结束时间；设备结束但 mailbox 错误时原因仍显示 kernel complete。
              <<",\"cause\":\""<<(demo.done()?"CoralNPU: kernel complete":"watchdog timeout")<<"\",\"tick_fs\":"
              // 设备结束时记录实际完成刻度，否则记录当前仿真刻度；同时写入设备周期数和十进制 mailbox 值。
              <<(demo.done()?demo.doneTick():sc_time_stamp().value())<<",\"cycles\":"<<demo.cycles()<<",\"mailbox\":"<<demo.mailbox()<<"}\n";
        // 成功时调用 finish 收尾存储记录、监控统计及波形；随后无条件返回结果码，失败为 1。
        if(passed)demo.finish();return passed?0:1;
    // 捕获 try 范围内派生自 std::exception 的异常，统一输出失败信息。
    } catch(const std::exception& e) {
        // 将异常的具体原因打印到标准错误；常规启动脚本会将其重定向到 run.log。
        std::cerr<<"Native simulation failed: "<<e.what()<<'\n';
        // 尝试写入异常失败状态；文件中仅保存通用原因，具体异常内容需查看日志。
        std::ofstream(directory+"/completion.json")<<"{\"passed\":false,\"code\":1,\"cause\":\"simulation exception; see run.log\"}\n";
        // 发生异常时返回非零退出码 1。
        return 1;
    // 结束异常处理分支。
    }
// 结束 SystemC 入口函数。
}
