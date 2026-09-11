#pragma once
#include <systemc>

// AXI4 subset: 64-bit address, 256-bit data, 16-bit IDs, INCR.
// Lists define real sc_in/sc_out ports and connecting sc_signal objects.
namespace storage_axi {
constexpr unsigned DataBits = 256;
constexpr unsigned DataBytes = DataBits / 8;
constexpr unsigned DataSize = 5;
using Data = sc_dt::sc_biguint<DataBits>;
}
#define AXI_M2S(X) \
 X(sc_dt::sc_uint<64>, awaddr) X(sc_dt::sc_uint<16>, awid) \
 X(sc_dt::sc_uint<8>, awlen) X(sc_dt::sc_uint<3>, awsize) \
 X(sc_dt::sc_uint<2>, awburst) X(bool, awvalid) \
 X(storage_axi::Data, wdata) X(sc_dt::sc_uint<32>, wstrb) \
 X(bool, wlast) X(bool, wvalid) X(bool, bready) \
 X(sc_dt::sc_uint<64>, araddr) X(sc_dt::sc_uint<16>, arid) \
 X(sc_dt::sc_uint<8>, arlen) X(sc_dt::sc_uint<3>, arsize) \
 X(sc_dt::sc_uint<2>, arburst) X(bool, arvalid) X(bool, rready)
#define AXI_S2M(X) \
 X(bool, awready) X(bool, wready) X(sc_dt::sc_uint<16>, bid) \
 X(sc_dt::sc_uint<2>, bresp) X(bool, bvalid) X(bool, arready) \
 X(sc_dt::sc_uint<16>, rid) X(storage_axi::Data, rdata) \
 X(sc_dt::sc_uint<2>, rresp) X(bool, rlast) X(bool, rvalid)

namespace storage_axi {
struct Signals {
#define FIELD(T, n) sc_core::sc_signal<T> n{#n};
    AXI_M2S(FIELD) AXI_S2M(FIELD)
#undef FIELD
    void trace(sc_core::sc_trace_file* f) {
#define TRACE(T, n) sc_core::sc_trace(f, n, #n);
        AXI_M2S(TRACE) AXI_S2M(TRACE)
#undef TRACE
    }
};
struct MasterPorts {
#define OUT(T, n) sc_core::sc_out<T> n{#n};
#define IN(T, n) sc_core::sc_in<T> n{#n};
    AXI_M2S(OUT) AXI_S2M(IN)
#undef OUT
#undef IN
    void bind(Signals& s) {
#define BIND(T, n) n(s.n);
        AXI_M2S(BIND) AXI_S2M(BIND)
#undef BIND
    }
};
struct SlavePorts {
#define OUT(T, n) sc_core::sc_out<T> n{#n};
#define IN(T, n) sc_core::sc_in<T> n{#n};
    AXI_M2S(IN) AXI_S2M(OUT)
#undef OUT
#undef IN
    void bind(Signals& s) {
#define BIND(T, n) n(s.n);
        AXI_M2S(BIND) AXI_S2M(BIND)
#undef BIND
    }
};
}
