#include "api.h"
#include "processor.h"
#include "types.h"
#include "mem.h"
#include <VX_types.h>
#include <cstring>
#include <stdexcept>

namespace ss {
struct Vortex::Impl {
    vortex::RAM ram{0, 4096};
    vortex::Processor proc;
    explicit Impl(std::function<bool(const VortexRequest&)> issue) {
        proc.attach_ram(&ram);
        proc.set_mem_timing_hook([issue](uint64_t token, const vortex::MemReq& req,
                                        const uint8_t* bytes, uint64_t byteen, uint32_t size) {
            if (req.op != vortex::MemOp::LD && req.op != vortex::MemOp::ST)
                throw std::runtime_error("unsupported external memory operation");
            if (size > 64) throw std::runtime_error("VORTEX memory block exceeds adapter");
            VortexRequest r{};
            r.token=token; r.address=req.addr & ~uint64_t(size-1);
            r.byteen=byteen; r.write=req.is_write(); r.size=size;
            if (r.write) {
                if (!bytes) throw std::runtime_error("write without payload");
                std::memcpy(r.data, bytes, size);
            }
            return issue(r);
        });
    }
};
Vortex::Vortex(std::function<bool(const VortexRequest&)> issue)
    : impl_(std::make_unique<Impl>(std::move(issue))) {}
Vortex::~Vortex() = default;
void Vortex::start(uint32_t pc, uint32_t arg) {
    auto& p=impl_->proc;
    p.dcr_write(VX_DCR_KMU_STARTUP_ADDR0,pc);
    p.dcr_write(VX_DCR_KMU_STARTUP_ARG0,arg);
    p.dcr_write(VX_DCR_KMU_STARTUP_ARG1,0);
    p.dcr_write(VX_DCR_KMU_GRID_DIM_X,1); p.dcr_write(VX_DCR_KMU_GRID_DIM_Y,1);
    p.dcr_write(VX_DCR_KMU_GRID_DIM_Z,1); p.dcr_write(VX_DCR_KMU_BLOCK_DIM_X,1);
    p.dcr_write(VX_DCR_KMU_BLOCK_DIM_Y,1); p.dcr_write(VX_DCR_KMU_BLOCK_DIM_Z,1);
    p.dcr_write(VX_DCR_KMU_LMEM_SIZE,0); p.dcr_write(VX_DCR_KMU_BLOCK_SIZE,1);
    p.dcr_write(VX_DCR_KMU_WARP_STEP_X,VX_CFG_NUM_THREADS);
    p.dcr_write(VX_DCR_KMU_WARP_STEP_Y,0); p.dcr_write(VX_DCR_KMU_WARP_STEP_Z,0);
    p.dcr_write(VX_DCR_KMU_CLUSTER_DIM_X,1); p.dcr_write(VX_DCR_KMU_CLUSTER_DIM_Y,1);
    p.dcr_write(VX_DCR_KMU_CLUSTER_DIM_Z,1);
}
bool Vortex::cycle() {return impl_->proc.cycle();}
void Vortex::complete(uint64_t token,const uint8_t* data,uint32_t size) {
    impl_->proc.complete_mem_timing(token,data,size);
}
}
