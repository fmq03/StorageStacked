/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SW_DEVICE_LIB_SPI_DISPLAY_DEV_CONFIG_H_
#define SW_DEVICE_LIB_SPI_DISPLAY_DEV_CONFIG_H_

#include <stdint.h>
#include <stdlib.h>

#ifndef UBYTE
#define UBYTE uint8_t
#endif  // UBYTE

#ifndef UWORD
#define UWORD uint16_t
#endif  // UWORD

// Sentinel value, SPI_HOST handles CS
#define DEV_CS_PIN 0
#define DEV_DC_PIN 16
#define DEV_RST_PIN 17

#if defined(__cplusplus)
extern "C" {
#endif

void DEV_Digital_Write(void* ctx, UWORD Pin, UBYTE Value);
void DEV_SPI_WRITE(void* ctx, UBYTE Value);
void DEV_SPI_BLOCK_WRITE(void* ctx, const uint8_t* buffer, size_t bytes);
void DEV_Delay_ms(void* ctx, UWORD Value);

#if defined(__cplusplus)
}
#endif

#endif  // SW_DEVICE_EXAMPLES_SPI_DISPLAY_DEV_CONFIG_H_
