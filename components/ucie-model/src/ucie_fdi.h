// ucie_fdi.h - public transaction-level FDI contract for the UCIe link model.
#pragma once

#include <systemc.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

enum class BusinessKind { Request, Response };

// Observable state of the behavioral link. Reset is the initial delta-cycle
// value, Training covers train_ui, Active means traffic may flow, Degraded
// records a CDR/deskew impairment, and Failed records watchdog termination.
enum class LinkState { Reset, Training, Active, Degraded, Failed };

inline const char* to_string(BusinessKind kind) {
    return kind == BusinessKind::Request ? "request" : "response";
}

inline const char* to_string(LinkState state) {
    switch (state) {
    case LinkState::Reset: return "reset";
    case LinkState::Training: return "training";
    case LinkState::Active: return "active";
    case LinkState::Degraded: return "degraded";
    case LinkState::Failed: return "failed";
    }
    return "unknown";
}

inline std::ostream& operator<<(std::ostream& os, LinkState state) {
    return os << to_string(state);
}

// Public FDI transaction. All four UcieLink data ports use this same type.
// payload is padded to Config::payload_bytes(); valid_bytes identifies the
// caller-owned bytes. IDs, VC, kind, and timestamps are transaction metadata
// and are intentionally not subjected to PHY corruption in this TLM model.
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes = 0;
    std::uint64_t transaction_id = 0;
    std::uint8_t vc = 0;
    BusinessKind kind = BusinessKind::Request;
    sc_core::sc_time transaction_start;
    sc_core::sc_time fdi_time;
};

inline std::ostream& operator<<(std::ostream& os, const FdiFlit& flit) {
    return os << "FdiFlit{id=" << flit.transaction_id << ",vc="
              << static_cast<unsigned>(flit.vc) << ",kind=" << to_string(flit.kind)
              << ",bytes=" << flit.valid_bytes << "}";
}
