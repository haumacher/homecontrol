/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
package org.homecontrol.spike;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import org.junit.jupiter.api.Test;

import com.dylibso.chicory.runtime.ExportFunction;
import com.dylibso.chicory.runtime.HostFunction;
import com.dylibso.chicory.runtime.ImportValues;
import com.dylibso.chicory.runtime.Instance;
import com.dylibso.chicory.wasm.Parser;
import com.dylibso.chicory.wasm.WasmModule;
import com.dylibso.chicory.wasm.types.FunctionType;
import com.dylibso.chicory.wasm.types.ValType;

/**
 * The Java side of the WebAssembly block spike (issue #59, question 8): the room controller's
 * .wasm, the very files the device and the host test run, replayed in Chicory against
 * fixtures/series.csv; every output and state bit must equal fixtures/expected.csv, which the
 * host test (WAMR) checks too.
 */
class BlockReplayTest {

	private static final Path DIR = Path.of(System.getProperty("spike.dir", ".."));

	/** What the block sees through its imports (the host side of abi/hc_block.h). */
	static final class Ports {
		final double[] in = new double[4];
		final int[] quality = new int[4];
		final double[] param = { 2.0, 90.0, 5.0, 30.0 };
		final double[] out = new double[4];
		long now;
		final List<String> log = new ArrayList<>();
	}

	private static Instance instantiate(WasmModule module, Ports p) {
		FunctionType iToF = FunctionType.of(List.of(ValType.I32), List.of(ValType.F64));
		ImportValues imports = ImportValues.builder()
			.addFunction(new HostFunction("hc", "now", FunctionType.returning(ValType.I64),
				(inst, args) -> new long[] { p.now }))
			.addFunction(new HostFunction("hc", "read", iToF,
				(inst, args) -> new long[] { Double.doubleToRawLongBits(port(args[0]) ? p.in[(int) args[0]] : Double.NaN) }))
			.addFunction(new HostFunction("hc", "quality", FunctionType.of(List.of(ValType.I32), List.of(ValType.I32)),
				(inst, args) -> new long[] { port(args[0]) ? p.quality[(int) args[0]] : 2 }))
			.addFunction(new HostFunction("hc", "param", iToF,
				(inst, args) -> new long[] { Double.doubleToRawLongBits(port(args[0]) ? p.param[(int) args[0]] : Double.NaN) }))
			.addFunction(new HostFunction("hc", "write", FunctionType.of(List.of(ValType.I32, ValType.F64), List.of()),
				(inst, args) -> {
					if (port(args[0])) {
						p.out[(int) args[0]] = Double.longBitsToDouble(args[1]);
					}
					return null;
				}))
			.addFunction(new HostFunction("hc", "log", FunctionType.of(List.of(ValType.I32, ValType.I32), List.of()),
				(inst, args) -> {
					p.log.add(inst.memory().readString((int) args[0], (int) args[1]));
					return null;
				}))
			.build();
		return Instance.builder(module).withImportValues(imports).build();
	}

	private static boolean port(long n) {
		return n >= 0 && n < 4;
	}

	private static double number(String text) {
		return text.equals("nan") ? Double.NaN : Double.parseDouble(text);
	}

	/** Replays the series through instance `self` and returns the rows as expected.csv writes them. */
	private static List<String> replay(String wasm, int self) throws IOException {
		WasmModule module = Parser.parse(DIR.resolve(wasm));
		Ports p = new Ports();
		Instance instance = instantiate(module, p);
		ExportFunction start = instance.export("hc_start");
		ExportFunction input = instance.export("hc_input");
		ExportFunction period = instance.export("hc_period");
		int state = (int) instance.export("hc_state").apply(self)[0];

		List<String> lines = Files.readAllLines(DIR.resolve("fixtures/series.csv"));
		List<String> rows = new ArrayList<>();
		boolean started = false;
		for (String line : lines.subList(1, lines.size())) {
			if (line.isEmpty()) {
				continue;
			}
			String[] f = line.split(",");
			long t = Long.parseLong(f[0]);
			p.in[0] = number(f[2]);
			p.quality[0] = Integer.parseInt(f[3]);
			p.in[1] = number(f[4]);
			p.in[2] = number(f[5]);
			p.now = t;
			if (!started) {
				start.apply(self, 0, 0);
				started = true;
			}
			if (f[1].equals("p")) {
				period.apply(self);
			} else {
				input.apply(self, Long.parseLong(f[1]));
			}
			rows.add(String.format("%d,%016x,%016x,%016x", t, Double.doubleToRawLongBits(p.out[0]),
				instance.memory().readLong(state), instance.memory().readLong(state + 8)));
		}
		assertTrue(p.log.get(0).startsWith("room controller started"), "the block's log line arrives");
		return rows;
	}

	private static List<String> expected() throws IOException {
		List<String> lines = Files.readAllLines(DIR.resolve("fixtures/expected.csv"));
		return lines.subList(1, lines.size());
	}

	@Test
	void cBlockGivesTheBitsOfTheDeviceAndTheHost() throws IOException {
		assertEquals(expected(), replay("wasm/room_controller.wasm", 0));
	}

	@Test
	void rustBlockGivesTheSameBits() throws IOException {
		assertEquals(expected(), replay("wasm/room_controller_rs.wasm", 0));
	}

	@Test
	void zigBlockGivesTheSameBits() throws IOException {
		assertEquals(expected(), replay("wasm/room_controller_zig.wasm", 0));
	}

	@Test
	void anotherInstanceOfTheModuleGivesTheSameBits() throws IOException {
		assertEquals(expected(), replay("wasm/room_controller.wasm", 7));
	}

	/**
	 * The faults of the probes trap in Chicory as on the device; the endless loop has no
	 * instruction budget in Chicory, but the interpreter checks the thread's interruption, so the
	 * simulation bounds a call by time.
	 */
	@Test
	void faultsTrapAndAnEndlessLoopIsInterrupted() throws Exception {
		WasmModule module = Parser.parse(DIR.resolve("wasm/probes.wasm"));
		for (String fault : List.of("oob", "stack_overflow", "shadow_stack_overflow", "unreachable", "trap", "div_zero")) {
			Instance instance = instantiate(module, new Ports());
			RuntimeException trap = org.junit.jupiter.api.Assertions.assertThrows(RuntimeException.class,
				() -> instance.export(fault).apply());
			System.out.println("chicory " + fault + ": " + trap.getClass().getSimpleName() + ": " + trap.getMessage());
		}

		Instance instance = instantiate(module, new Ports());
		java.util.concurrent.ExecutorService executor = java.util.concurrent.Executors.newSingleThreadExecutor();
		try {
			java.util.concurrent.Future<long[]> call = executor.submit(() -> instance.export("loop_forever").apply());
			long begin = System.nanoTime();
			try {
				call.get(50, java.util.concurrent.TimeUnit.MILLISECONDS);
			} catch (java.util.concurrent.TimeoutException expected) {
				call.cancel(true);
			}
			java.util.concurrent.ExecutionException stopped = org.junit.jupiter.api.Assertions.assertThrows(
				java.util.concurrent.ExecutionException.class, () -> {
					try {
						call.get();
					} catch (java.util.concurrent.CancellationException cancelled) {
						// The future reports the cancellation; wait for the thread to leave the module.
						executor.shutdown();
						assertTrue(executor.awaitTermination(1, java.util.concurrent.TimeUnit.SECONDS),
							"the interpreter leaves the endless loop when interrupted");
						throw new java.util.concurrent.ExecutionException("interrupted", cancelled);
					}
				});
			System.out.println("chicory loop_forever: left after "
				+ (System.nanoTime() - begin) / 1000000 + " ms: " + stopped.getMessage());
		} finally {
			executor.shutdownNow();
		}
	}
}
