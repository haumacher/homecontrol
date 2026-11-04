# Copyright (c) 2026 Bernhard Haumacher
#
# The ESP-IDF's RISC-V compiler (riscv32-esp-elf, on the PATH after `source ~/tools/esp/esp-idf-v5.5/export.sh`)
# for a program that runs under qemu-riscv32 user mode (newlib's `sim` system calls): the 32-bit
# model of the core's heap on the ESP32-C6 (doc/image.md "Memory of a swap").
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER riscv32-esp-elf-gcc)
set(CMAKE_CXX_COMPILER riscv32-esp-elf-g++)
set(CMAKE_C_FLAGS_INIT "-march=rv32imac_zicsr_zifencei -mabi=ilp32")
set(CMAKE_CXX_FLAGS_INIT "-march=rv32imac_zicsr_zifencei -mabi=ilp32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=sim.specs")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
