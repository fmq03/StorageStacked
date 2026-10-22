// 引入主端适配器的类声明和依赖类型。
#include "axi_master.hh"
// 引入数据复制和最大值比较算法。
#include <algorithm>
// 引入十六进制字节输出所需的宽度与填充控制。
#include <iomanip>
// 引入运行时异常和非法参数异常。
#include <stdexcept>
// 将主端实现放入 storage_axi 命名空间。
namespace storage_axi {
// 使用 SystemC 核心命名空间，简化模块和时间接口名称。
using namespace sc_core;
// 构造 SystemC 基类，并保存运行配置副本。
Master::Master(sc_module_name name,const ConfigParams& p):sc_module(name),config(p),
 // 按设备周期创建独立的 NPU 时钟。
 nativeClock("npu_clock",sc_time::from_value(p.device_period)),
 // 打开通用事务日志和原生请求/响应日志，进入构造函数体。
 trace(p.trace_dir+"/transactions.csv"),source(p.trace_dir+"/npu_requests.csv") {
    // 任一日志文件打开失败时抛出异常，终止初始化。
    if(!trace || !source) throw std::runtime_error("Cannot open native request logs");
    // 检查 burst 拍数非零、不超过 16，且只能是 2 的幂。
    if(!p.burst_beats || p.burst_beats>16 || (p.burst_beats&(p.burst_beats-1)))
        // 拍数非法时报告仅支持 1、2、4、8 或 16 拍。
        throw std::invalid_argument("axi.burst_beats must be 1, 2, 4, 8 or 16");
    // 将固定 16 字节设备请求均分，计算每个 AXI 数据拍的字节数。
    beatBytes=16/p.burst_beats;
    // 计算每拍字节数的 log2，作为 AXI 的 AxSIZE 编码。
    while((1u<<beatSize)<beatBytes) ++beatSize;
    // 写入通用事务 CSV 表头，包含各阶段时间与请求属性。
    trace<<"id,command,address,bytes,begin_tick,accepted_tick,axi_done_tick,end_resp_tick,segments,requestor,stream,substream,status\n";
    // 写入原生请求 CSV 表头，包含事件、序号、双侧 ID、数据、掩码和响应。
    source<<"event,tick,sequence,command,address,native_id,wire_id,data,mask,response\n";
    // 创建原生模型，并注册携带序号、地址、ID、读写方向、数据和掩码的请求回调。
    device=coralnpu_create([](void* p,uint64_t seq,uint32_t addr,uint8_t id,int write,const uint8_t* data,uint16_t mask){
        // 将回调上下文转回 Master 指针，把原生访问交给 submit。
        static_cast<Master*>(p)->submit(seq,addr,id,write,data,mask);
    // 注册容量查询回调：在途事务数量小于 outstanding 时才允许接收。
    },[](void* p){auto& self=*static_cast<Master*>(p);bool ready=self.active.size()<self.config.outstanding;
        // 容量不足时累计拒绝次数；始终返回当前就绪状态，并将 this 用作回调上下文。
        if(!ready) ++self.capacityDenials;return int(ready);},this);
    // 检查设备创建与 ELF 加载结果，失败时抛出运行时异常。
    if(!device || coralnpu_load(device,p.kernel.c_str())) throw std::runtime_error("Cannot load CoralNPU ELF");
    // 注册 AXI 采样方法，仅在 clk 上升沿执行，跳过初始化调用。
    SC_METHOD(tick);sensitive<<clk.pos();dont_initialize();
    // 注册原生模型推进方法，仅在设备时钟上升沿执行，跳过初始化调用。
    SC_METHOD(nativeTick);sensitive<<nativeClock.posedge_event();dont_initialize();
// 结束主端构造函数。
}
// 析构适配器时释放原生设备模型。
Master::~Master(){coralnpu_destroy(device);}
// 定义向 npu_requests.csv 写一条事件记录的辅助函数。
void Master::sourceRow(const char* event,const Txn& t) {
    // 输出事件名、当前 fs 时间、原生序号及读写方向。
    source<<event<<','<<sc_time_stamp().value()<<','<<t.seq<<','<<(t.write?'W':'R')<<','
          // 继续输出访问地址、原生 ID 与分配的 AXI ID。
          <<t.address<<','<<unsigned(t.nativeId)<<','<<t.id<<',';
    // 写请求或已完成的读请求才输出数据；每字节以两位补零十六进制表示。
    if(t.write || t.ready) for(auto byte:t.data) source<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
    // 切回十进制输出掩码和响应码，结束该行并立即刷新日志。
    source<<std::dec<<','<<t.mask<<','<<unsigned(t.response)<<'\n';source.flush();
// 结束原生事件记录函数。
}
// 定义原生请求入口，接收一笔 16 字节访问的元数据与数据。
void Master::submit(uint64_t seq,uint32_t addr,uint8_t nativeId,bool write,const uint8_t* data,uint16_t mask) {
    // 要求在途容量充足且地址按 16 字节对齐，否则立即报错。
    if(active.size()>=config.outstanding || (addr&15)) throw std::runtime_error("Native request exceeds adapter capacity/alignment");
    // 跳过仍在使用的 AXI ID，在 1 到 1023 范围内循环查找。
    while(active.count(nextId)) nextId=nextId==1023?1:nextId+1;
    // 保存本次分配的 ID，并将候选 ID 前移，1023 后回到 1。
    auto id=nextId;nextId=nextId==1023?1:nextId+1;
    // 零初始化事务，填入原生序号、地址、双侧 ID、掩码及读写方向。
    Txn t{};t.seq=seq;t.address=addr;t.id=id;t.mask=mask;t.nativeId=nativeId;t.write=write;
    // 将事务开始和适配器接收时刻都记为当前 SystemC 时间。
    t.begin=t.accept=sc_time_stamp().value();
    // 对写请求复制 16 字节原生数据，读请求保留初始缓冲区。
    if(write) std::copy(data,data+16,t.data.begin());
    // 启用 stalls 且 ID 为奇数时，将 AW 最早发送周期延后 3 拍。
    t.awAfter=cycle+(config.stalls && (id&1)?3:0);
    // 启用 stalls 且 ID 为偶数时，将 W 最早发送周期延后 3 拍。
    t.wAfter=cycle+(config.stalls && !(id&1)?3:0);
    // 插入在途表，并加入对应读写方向与原生 ID 的响应顺序队列。
    active.emplace(id,t);order[{write,nativeId}].push_back(id);
    // 写请求分别进入 AW 和 W 队列；读请求进入 AR 队列。
    if(write){awq.push_back(id);wq.push_back(id);}else arq.push_back(id);
    // 累计接收数、更新在途峰值，并写入原生 request 事件。
    ++accepted;maxActive=std::max<uint64_t>(maxActive,active.size());sourceRow("request",t);
// 结束原生请求接收函数。
}
// 定义已完成事务向原生模型回送的处理函数。
void Master::retire() {
    // 遍历各读写方向/原生 ID 组，仅连续处理队首已经 ready 的事务。
    for(auto& [key,ids]:order) while(!ids.empty() && active.at(ids.front()).ready) {
        // 复制队首事务，便于稍后从在途表中删除原记录。
        auto t=active.at(ids.front());
        // 将完成数据和响应码连同原生序号、ID 回送设备模型。
        coralnpu_complete(device,t.seq,t.nativeId,t.write,t.data.data(),t.response);
        // 在原生日志中记录 response 事件。
        sourceRow("response",t);
        // 写入 AXI ID、读写方向、地址、固定 16 字节长度及开始/接收时刻。
        trace<<t.id<<','<<(t.write?'W':'R')<<','<<t.address<<",16,"<<t.begin<<','<<t.accept<<','
             // 继续写入 AXI 完成和回送时间、单段标记、原生属性及状态；响应非零映射为状态 2，否则为 1。
             <<t.axiDone<<','<<sc_time_stamp().value()<<",1,0,"<<unsigned(t.nativeId)<<','<<t.seq<<','<<(t.response?2:1)<<'\n';
        // 从在途表与保序队列移除该事务，并累计完成数。
        active.erase(t.id);ids.pop_front();++completed;
    // 结束当前保序组内可回送事务的处理循环。
    }
    // 将本次写出的通用事务记录刷新到文件。
    trace.flush();
// 结束事务回送函数。
}
// 定义在设备时钟上升沿运行的原生模型推进方法。
void Master::nativeTick() {
    // 复位尚未释放或设备已经结束时，不推进设备。
    if(!resetn.read() || done) return;
    // 先把已完成的外部访问按原生 ID 顺序回送设备。
    retire();
    // 推进一步设备模型；返回零表示设备执行停止。
    if(!coralnpu_step(device)) {
        // 若设备停止时仍有未回送的外部访问，则报告执行状态错误。
        if(!idle()) throw std::runtime_error("NPU stopped with incomplete external accesses");
        // 标记设备结束，并保存实际结束的 SystemC 时间刻度。
        done=true;doneTick=sc_time_stamp().value();
        // 打印设备周期数和十六进制 mailbox，再恢复十进制输出格式。
        std::cout<<"CoralNPU kernel finished after "<<cycles()<<" cycles, mailbox="<<std::hex<<mailbox()<<std::dec<<'\n';
    // 结束设备停止后的处理分支。
    }
// 结束设备时钟方法。
}
// 定义 AXI 时钟沿的握手采样与状态更新方法。
void Master::tick() {
    // 递增 AXI 周期计数，作为确定性延迟和反压的基准。
    ++cycle;
    // 复位期间清除各请求有效信号、WLAST 和响应就绪信号，然后结束本次处理。
    if(!resetn.read()) {axi.awvalid=false;axi.wvalid=false;axi.wlast=false;axi.arvalid=false;axi.rready=false;axi.bready=false;return;}
    // AW 握手时确认写地址队列非空，并移除已经发送的队首 ID。
    if(axi.awvalid.read() && axi.awready.read()){sc_assert(!awq.empty());awq.pop_front();}
    // 仅当 WVALID 与 WREADY 同时为真时，处理一个已发送写数据拍。
    if(axi.wvalid.read() && axi.wready.read()){
        // 确认写数据队列非空，并取得队首事务。
        sc_assert(!wq.empty());auto& t=active.at(wq.front());
        // 检查写拍计数未超出配置的 burst 长度。
        sc_assert(t.wbeat<config.burst_beats);
        // 增加已发送写拍数，最后一拍握手后从 W 队列移除事务。
        if(++t.wbeat==config.burst_beats) wq.pop_front();
    // 结束写数据握手处理分支。
    }
    // AR 握手时确认读地址队列非空，并移除已经发送的读地址 ID。
    if(axi.arvalid.read() && axi.arready.read()){sc_assert(!arq.empty());arq.pop_front();}
    // 仅当 RVALID 与 RREADY 同时为真时，接收一个读响应拍。
    if(axi.rvalid.read() && axi.rready.read()) {
        // 按 RID 找到该读响应所属的在途事务。
        auto& t=active.at(axi.rid.read().to_uint());
        // 确保目标为未完成的读请求，且尚有待读取的数据拍。
        sc_assert(!t.write && !t.ready && t.rbeat<config.burst_beats);
        // 检查 RLAST 只在预期的最后一个读拍置位。
        sc_assert(axi.rlast.read()==(t.rbeat+1==config.burst_beats));
        // 计算当前拍在 16 字节缓冲中的偏移，以及在 32 字节 AXI 总线中的字节 lane。
        const unsigned offset=t.rbeat*beatBytes,lane=(t.address+offset)%32;
        // 读取当前 AXI 读数据总线值。
        auto data=axi.rdata.read();
        // 将本拍有效 lane 内的各字节抽取到原生读缓冲区。
        for(unsigned j=0;j<beatBytes;++j)t.data[offset+j]=data.range(8*(lane+j)+7,8*(lane+j)).to_uint();
        // 按位或累计各读拍的响应码，保留任何非零错误状态。
        t.response|=axi.rresp.read().to_uint();
        // 读拍收齐时标记 ready，并记录 AXI 完成时刻，等待设备时钟沿回送。
        if(++t.rbeat==config.burst_beats){t.ready=true;t.axiDone=sc_time_stamp().value();}
    // 结束读数据响应处理分支。
    }
    // 仅当 BVALID 与 BREADY 同时为真时，接收写响应。
    if(axi.bvalid.read() && axi.bready.read()) {
        // 按 BID 找到对应的在途写事务。
        auto& t=active.at(axi.bid.read().to_uint());
        // 确保它是未完成的写请求，且配置的写数据拍均已发送。
        sc_assert(t.write && !t.ready && t.wbeat==config.burst_beats);
        // 保存写响应码、标记 ready，并记录 AXI 完成时刻。
        t.response=axi.bresp.read().to_uint();t.ready=true;t.axiDone=sc_time_stamp().value();
    // 结束写响应处理分支。
    }
    // 根据更新后的队列和事务状态驱动后续 AXI 输出。
    drive();
// 结束 AXI 时钟沿处理方法。
}
// 定义按通道队首事务设置 AXI 输出的方法。
void Master::drive() {
    // 默认撤销 AWVALID，只有存在待发写地址时才重新使能。
    axi.awvalid=false;
    // 队列非空时输出写地址、ID、LEN、SIZE 和 INCR burst 类型，并按延迟阈值设置 AWVALID。
    if(!awq.empty()){const auto& t=active.at(awq.front());axi.awaddr=t.address;axi.awid=t.id;axi.awlen=config.burst_beats-1;axi.awsize=beatSize;axi.awburst=1;axi.awvalid=cycle>=t.awAfter;}
    // 默认撤销 ARVALID，避免空队列发送读地址。
    axi.arvalid=false;
    // 读队列非空时输出读地址、ID 和 burst 参数，并使能 ARVALID。
    if(!arq.empty()){const auto& t=active.at(arq.front());axi.araddr=t.address;axi.arid=t.id;axi.arlen=config.burst_beats-1;axi.arsize=beatSize;axi.arburst=1;axi.arvalid=true;}
    // 默认撤销 WVALID 与 WLAST，等待待发写数据填充。
    axi.wvalid=false;axi.wlast=false;
    // 写数据队列非空时准备当前写拍。
    if(!wq.empty()){
        // 取得队首写事务，并将本拍的整条 AXI 数据总线初始清零。
        const auto& t=active.at(wq.front());Data data=0;
        // 计算当前写拍在原生数据中的偏移和在 32 字节总线中的起始 lane。
        const unsigned offset=t.wbeat*beatBytes,lane=(t.address+offset)%32;
        // 把当前拍的原生数据逐字节装入 AXI 总线对应 lane。
        for(unsigned j=0;j<beatBytes;++j)data.range(8*(lane+j)+7,8*(lane+j))=t.data[offset+j];
        // 将原生 16 位字节掩码右移到当前拍，并截取本拍有效字节范围。
        const uint32_t mask=(uint32_t(t.mask)>>offset)&((1u<<beatBytes)-1);
        // 输出数据、按 lane 平移后的 WSTRB、末拍 WLAST，并按阈值设置 WVALID。
        axi.wdata=data;axi.wstrb=mask<<lane;axi.wlast=t.wbeat+1==config.burst_beats;axi.wvalid=cycle>=t.wAfter;
    // 结束写数据拍的准备分支。
    }
    // 无 stalls 时持续接收读响应；启用时每 7 个 AXI 周期中后 4 个周期允许接收。
    axi.rready=!config.stalls || cycle%7>=3;
    // 无 stalls 时持续接收写响应；启用时每 5 个 AXI 周期中后 3 个周期允许接收。
    axi.bready=!config.stalls || cycle%5>=2;
// 结束 AXI 输出驱动方法。
}
// 结束 storage_axi 命名空间。
}
