/*
 * Chip-specific extensions for CoralNPU.
 * FreeRTOS RISC-V port requires this file to exist.
 */
#ifndef FREERTOS_RISC_V_CHIP_SPECIFIC_EXTENSIONS_H
#define FREERTOS_RISC_V_CHIP_SPECIFIC_EXTENSIONS_H

/* No additional registers to save/restore on this core beyond standard RISC-V.
 */
/* clang-format off */
.macro portasmSAVE_ADDITIONAL_REGISTERS
.endm

.macro portasmRESTORE_ADDITIONAL_REGISTERS
.endm
/* clang-format on */

#define portasmADDITIONAL_CONTEXT_SIZE 0

/* CoralNPU subsystem has a CLINT with MTIME/MTIMECMP. */
#define portasmHAS_MTIME 1

#endif /* FREERTOS_RISC_V_CHIP_SPECIFIC_EXTENSIONS_H */
