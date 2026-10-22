#pragma once

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include "sim/core.hh"

namespace gem5::X86ISA {
// A bounded sample of completed translations, without changing MMU behavior.
inline void traceTranslation(const std::string& tlb, int context,
                             Addr virtualAddress, Addr physicalAddress, int mode)
{
    static const char* path = std::getenv("SOC_MMU_TRACE");
    if (!path) return;
    static std::ofstream output(path);
    static bool header = false;
    static std::map<int, std::set<std::tuple<Addr, Addr, int>>> seen;
    auto& entries = seen[context];
    if (entries.size() >= 64 ||
        !entries.emplace(virtualAddress >> 12, physicalAddress >> 12, mode).second)
        return;
    if (!header) {
        output << "context,tlb,virtual_address,physical_address,mode,tick_fs\n";
        header = true;
    }
    output << context << ',' << tlb << ',' << virtualAddress << ','
           << physicalAddress << ',' << mode << ',' << curTick() << '\n';
    output.flush();
}
}
