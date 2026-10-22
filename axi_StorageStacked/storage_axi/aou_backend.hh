#pragma once
#include "axi_signals.hh"
#include "storage_config.hh"
#include <memory>

namespace storage_axi {
// Native 256-bit AXI signal/structure binding and the real AoU/UCIe path.
class AouBackend : public sc_core::sc_module {
  public:
    sc_core::sc_in<bool> clk{"clk"}, resetn{"resetn"};
    SlavePorts axi;
    AouBackend(sc_core::sc_module_name, const StorageConfig&);
    ~AouBackend();
    bool ready() const;
    void trace(sc_core::sc_trace_file*);
    void finish(const std::string&);
  private:
    struct Fabric;
    std::unique_ptr<Fabric> fabric;
};
}
