#pragma once
#include <systemc>
#include "simple_mem_if.h"
#include "online.h"
#include <deque>
#include <map>
#include <memory>
#include <fstream>

// Protocol bridge only: payload storage and completion timing belong to mem_sim.
class MemSimBackend : public sc_core::sc_module {
 public:
  sc_core::sc_fifo_in<SimpleMemRequest> request{"request"};
  sc_core::sc_fifo_out<SimpleMemResponse> response{"response"};
  uint64_t completed=0, error_responses=0;
  SC_HAS_PROCESS(MemSimBackend);
  MemSimBackend(sc_core::sc_module_name, uint64_t base, uint64_t size,
                unsigned slots, unsigned channels, unsigned scale,
                unsigned queue, unsigned response_hold, const std::string& dir);
  ~MemSimBackend();
  void finish();
 private:
  struct Burst {
   uint64_t serial=0, arrived=0, ready=0;
   SimpleMemRequest req;
   SimpleMemResponse rsp;
   std::vector<uint8_t> data, mask;
   unsigned length=0, sent=0, pending=0;
   uint64_t candidate=0;
   bool error=false;
  };
  struct Child { std::shared_ptr<Burst> burst; unsigned offset, bytes; };
  uint64_t base, period, nextBurst=1, nextMem=1, submitStalls=0, responseStalls=0;
  uint64_t size;
  unsigned slots, hold;
  ss_mem* mem;
  std::string dir;
  std::ofstream log;
  std::deque<std::shared_ptr<Burst>> bursts;
  std::map<uint64_t,Child> children;
  void run();
  void accept(SimpleMemRequest);
  void event(const char*, const Burst&, unsigned offset=0, unsigned bytes=0,
             uint64_t id=0, uint64_t issued=0, uint64_t completion=0,
             unsigned status=0, const uint8_t* data=nullptr, const uint8_t* mask=nullptr);
  void checked(int);
};
