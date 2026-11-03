#!/bin/sh
# Copyright (c) 2026 Bernhard Haumacher
#
# Builds the spike's blocks (issue #59, doc/blocks-wasm.md) into spike/blocks-wasm/wasm/:
#   room_controller.wasm   the PI test block in C (clang --target=wasm32, no libc, no WASI)
#   probes.wasm            the faulty calls and the benchmarks in C
#   bulk.wasm              about 20 KB of generated code, for the load measurements
#   room_controller_rs.wasm the same block in Rust (wasm32-unknown-unknown, no_std)
#   room_controller_zig.wasm the same block in Zig (wasm32-freestanding)
#   *.riscv32.aot          AOT for the ESP32-C6 (wamrc --target=riscv32 ilp32, XIP)
#   room_controller.noxip.riscv32.aot  the same without XIP (code copied to RAM at load)
#   room_controller.x86_64.aot, probes.mt.x86_64.aot  AOT for the host tests
# The outputs are committed: the same .wasm runs on the device, on the host and in Java, and the
# builds need no WebAssembly toolchain. The compilers are deterministic: a rebuild gives the same bytes.
#
# Tools (override with the variables):
#   CLANG   a clang with the wasm32 target and wasm-ld next to it (default: the WASI SDK 34's, whose
#           sysroot is not used)
#   WAMRC   wamrc of the WAMR release the runtime uses (2.4.5)
#   CARGO   cargo with the wasm32-unknown-unknown target (skipped if missing)
#   ZIG     zig 0.17 (skipped if missing)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/wasm"
CLANG=${CLANG:-$HOME/tools/wasi-sdk-34-dl/wasi-sdk-34.0-x86_64-linux/bin/clang}
WAMRC=${WAMRC:-$HOME/tools/wamrc-2.4.5-dl/wamrc}
CARGO=${CARGO:-$HOME/tools/cargo/bin/cargo}
ZIG=${ZIG:-$HOME/tools/zig-0.17.1-dl/zig-x86_64-linux-0.17.1/zig}
mkdir -p "$OUT"

# -mcpu=mvp: only the WebAssembly 1.0 instruction set (every runtime has it; newer LLVM defaults
#   add bulk memory, reference types, sign extension and multi-value).
# --stack-first: the C stack lies below the data, so an overflow leaves the memory and traps instead
#   of overwriting the block's state. 4 KB of stack.
# --initial-memory = --max-memory = one page and no memory.grow: WAMR shrinks such a memory to
#   __heap_base (WASM_ENABLE_SHRUNK_MEMORY), so an instance costs its data plus its stack, not 64 KB.
# Exporting __heap_base and __data_end is what lets WAMR find the layout.
CFLAGS="--target=wasm32 -mcpu=mvp -O2 -nostdlib -ffreestanding -fno-builtin -Wall -Wextra -Werror"
LDFLAGS="-Wl,--no-entry -Wl,--stack-first -Wl,-z,stack-size=4096 \
	-Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
	-Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--strip-all"

python3 "$HERE/blocks/c/gen_bulk.py" "$HERE/blocks/c/bulk.c"
for block in room_controller probes bulk; do
	"$CLANG" $CFLAGS $LDFLAGS -o "$OUT/$block.wasm" "$HERE/blocks/c/$block.c"
done

if [ -x "$CARGO" ]; then
	export RUSTUP_HOME=${RUSTUP_HOME:-$HOME/tools/rustup}
	PATH=$(dirname "$CARGO"):$PATH
	(cd "$HERE/blocks/rust/room-controller" &&
		"$CARGO" build --quiet --release --target wasm32-unknown-unknown \
			--target-dir "$HERE/blocks/rust/target")
	cp "$HERE/blocks/rust/target/wasm32-unknown-unknown/release/room_controller.wasm" \
		"$OUT/room_controller_rs.wasm"
else
	echo "cargo not found ($CARGO): the Rust block is not rebuilt" >&2
fi

if [ -x "$ZIG" ]; then
	ZIG_CACHE=$(mktemp -d)
	"$ZIG" build-exe "$HERE/blocks/zig/room_controller.zig" -target wasm32-freestanding -mcpu=mvp \
		-O ReleaseSmall -fno-entry -rdynamic --stack 4096 --initial-memory=65536 --max-memory=65536 \
		--export=__heap_base --export=__data_end -femit-bin="$OUT/room_controller_zig.wasm" \
		--cache-dir "$ZIG_CACHE" --global-cache-dir "$ZIG_CACHE"
	rm -rf "$ZIG_CACHE" "$OUT/room_controller_zig.wasm.o"
else
	echo "zig not found ($ZIG): the Zig block is not rebuilt" >&2
fi

# AOT for the C6 (the measurement app): rv32imac, ilp32 (soft float), XIP (no relocations in the
# code, so it runs in place from a flash mapping). For the host test: the room controller for x86-64.
for block in room_controller probes bulk; do
	"$WAMRC" --target=riscv32 --target-abi=ilp32 --cpu=generic-rv32 --cpu-features=+m,+a,+c \
		--xip --disable-bulk-memory --disable-ref-types \
		-o "$OUT/$block.riscv32.aot" "$OUT/$block.wasm" >/dev/null
done
"$WAMRC" --target=x86_64 --bounds-checks=1 --disable-bulk-memory --disable-ref-types --disable-simd \
	-o "$OUT/room_controller.x86_64.aot" "$OUT/room_controller.wasm" >/dev/null

# probes with thread-manager support, for the AOT termination test (host/thread-mgr).
"$WAMRC" --target=x86_64 --bounds-checks=1 --enable-multi-thread --disable-ref-types --disable-simd \
	-o "$OUT/probes.mt.x86_64.aot" "$OUT/probes.wasm" >/dev/null

# The same without XIP, to show that its code would need executable RAM.
"$WAMRC" --target=riscv32 --target-abi=ilp32 --cpu=generic-rv32 --cpu-features=+m,+a,+c \
	--disable-bulk-memory --disable-ref-types \
	-o "$OUT/room_controller.noxip.riscv32.aot" "$OUT/room_controller.wasm" >/dev/null

ls -l "$OUT"
