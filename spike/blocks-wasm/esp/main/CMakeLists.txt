# Copyright (c) 2026 Bernhard Haumacher
set(SPIKE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
idf_component_register(
	SRCS "spike_main.cpp" "${SPIKE_DIR}/runner/spike_runner.cpp"
	INCLUDE_DIRS "${SPIKE_DIR}/runner"
	REQUIRES wamr pthread esp_timer esp_partition spi_flash heap esp_system freertos littlefs vfs
	EMBED_FILES
		"${SPIKE_DIR}/wasm/room_controller.wasm"
		"${SPIKE_DIR}/wasm/room_controller_rs.wasm"
		"${SPIKE_DIR}/wasm/probes.wasm"
		"${SPIKE_DIR}/wasm/bulk.wasm"
		"${SPIKE_DIR}/wasm/room_controller.riscv32.aot"
		"${SPIKE_DIR}/wasm/probes.riscv32.aot"
		"${SPIKE_DIR}/wasm/bulk.riscv32.aot"
		"${SPIKE_DIR}/wasm/room_controller.noxip.riscv32.aot"
	EMBED_TXTFILES
		"${SPIKE_DIR}/fixtures/series.csv"
		"${SPIKE_DIR}/fixtures/expected.csv")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WASM_ENABLE_INSTRUCTION_METERING=1)
