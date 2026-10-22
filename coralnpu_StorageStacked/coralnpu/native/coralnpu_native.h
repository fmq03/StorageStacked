#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// A bounded asynchronous seam between the RTL AXI callbacks and the AXI256 adapter.
// No private external RAM, transaction simulator, or second SystemC kernel.
typedef struct coralnpu_device* coralnpu_handle;
typedef int (*coralnpu_ready_fn)(void*);
typedef void (*coralnpu_request_fn)(void*, uint64_t sequence, uint32_t address,
                                  uint8_t axi_id, int write,
                                  const uint8_t data[16], uint16_t strobe);
coralnpu_handle coralnpu_create(coralnpu_request_fn, coralnpu_ready_fn, void*);
void coralnpu_destroy(coralnpu_handle);
int coralnpu_load(coralnpu_handle, const char* elf);
int coralnpu_step(coralnpu_handle);
void coralnpu_complete(coralnpu_handle, uint64_t sequence, uint8_t id,
                      int write, const uint8_t data[16], uint8_t response);
uint32_t coralnpu_mailbox(coralnpu_handle);
uint64_t coralnpu_cycles(coralnpu_handle);
const char* coralnpu_configuration(void);
#ifdef __cplusplus
}
#endif
