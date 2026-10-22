// Copyright 2026 Google LLC
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef FPGA_SW_UART_H_
#define FPGA_SW_UART_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART0_BASE 0x40000000
#define UART1_BASE 0x40010000

uint32_t uart_get_base_addr(void);

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char* s);
void uart_puthex8(uint8_t v);
void uart_puthex32(uint32_t v);

#ifdef __cplusplus
}
#endif

#endif  // FPGA_SW_UART_H_
