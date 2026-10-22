#pragma once
#include <cstdint>
#include <string>
namespace storage_axi {
struct StorageConfig {
    uint64_t base=0x90000000, size=0x30000000;
    unsigned planes=2;
    bool replay=false;
    unsigned memsim_slots=8, memsim_channels=2, memsim_scale=1;
    unsigned memsim_queue=4, memsim_response_hold=0;
    std::string memory_backend="memsim", memsim_standard="hbm4";
    std::string trace_dir;
};
}
