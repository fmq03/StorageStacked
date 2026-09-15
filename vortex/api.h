#pragma once
#include <cstdint>
#include <functional>
#include <memory>

namespace ss {
struct VortexRequest {
    uint64_t token, address, byteen;
    bool write;
    uint8_t data[64];
    uint32_t size;
};
class Vortex {
public:
    explicit Vortex(std::function<bool(const VortexRequest&)> issue);
    ~Vortex();
    void start(uint32_t pc, uint32_t arg);
    bool cycle();
    void complete(uint64_t token, const uint8_t* data, uint32_t size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
