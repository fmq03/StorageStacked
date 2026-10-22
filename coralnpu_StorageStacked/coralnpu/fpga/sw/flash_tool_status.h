// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef FPGA_SW_FLASH_TOOL_STATUS_H_
#define FPGA_SW_FLASH_TOOL_STATUS_H_

typedef enum {
  FLASH_TOOL_STATUS_OK = 0,
  FLASH_TOOL_STATUS_DISCOVERY_FAILED = 1,
  FLASH_TOOL_STATUS_VERIFY_FAILED = 2,
  FLASH_TOOL_STATUS_CRC_MISMATCH = 3,
  FLASH_TOOL_STATUS_BOUNDARY_ERROR = 4,
  FLASH_TOOL_STATUS_BUFFER_TOO_SMALL = 5,
  FLASH_TOOL_STATUS_INVALID_LEN = 6,
  FLASH_TOOL_STATUS_NOT_INITIALIZED = 7,
  FLASH_TOOL_STATUS_ADDRESS_ERROR = 8,
  FLASH_TOOL_STATUS_TIMEOUT = 9,
} flash_tool_status_t;

#endif  // FPGA_SW_FLASH_TOOL_STATUS_H_
