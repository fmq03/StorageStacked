#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* No SystemC/C++ types cross this boundary. All calls run on gem5's thread. */
typedef struct ss_mem ss_mem;
typedef struct {
 uint64_t id, issued_cycle, completion_cycle;
 uint32_t bytes, status;
 uint8_t data[64];
} ss_mem_response;
ss_mem* ss_mem_create(const char* standard, unsigned channels, unsigned timing_scale,
                     unsigned queue_depth, uint64_t size, const char* outdir);
void ss_mem_destroy(ss_mem*);
const char* ss_mem_error(void);
uint64_t ss_mem_period_fs(ss_mem*);
uint64_t ss_mem_clock(ss_mem*);
unsigned ss_mem_transaction_bytes(ss_mem*);
int ss_mem_submit(ss_mem*, uint64_t id, uint64_t address, unsigned bytes,
                  int write, const uint8_t* data, const uint8_t* mask);
int ss_mem_step(ss_mem*);
/* 0 empty, 1 consumed, -1 error. A full external queue must not call pop. */
int ss_mem_pop(ss_mem*, ss_mem_response*);
int ss_mem_finish(ss_mem*);
#ifdef __cplusplus
}
#endif
