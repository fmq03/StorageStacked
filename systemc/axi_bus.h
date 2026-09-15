#pragma once
#include "axi2flit.h"
struct AxiBus {
    sc_signal<bool> av,ar,wv,wr,arv,arr,bv,br,rv,rr;
    sc_signal<AxChannel> aw,ra;
    sc_signal<WChannel> w;
    sc_signal<BChannel> b;
    sc_signal<RChannel> r;
    void bind(Axi2Flit& x) {
        x.aw_valid(av);x.aw_ready(ar);x.aw_ch(aw);x.w_valid(wv);x.w_ready(wr);x.w_ch(w);
        x.ar_valid(arv);x.ar_ready(arr);x.ar_ch(ra);x.b_valid(bv);x.b_ready(br);x.b_ch(b);
        x.r_valid(rv);x.r_ready(rr);x.r_ch(r);
    }
};
