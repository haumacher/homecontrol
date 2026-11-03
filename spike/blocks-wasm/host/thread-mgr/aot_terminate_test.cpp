/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * Stopping an endless loop in AOT code (issue #59, question 3). AOT code has no instruction
 * metering; the only stop WAMR offers is wasm_runtime_terminate() from another thread. This test
 * runs loop_forever of probes.mt.x86_64.aot (wamrc --enable-multi-thread, a runtime with the thread
 * manager) on a worker thread and calls wasm_runtime_terminate() after 20 ms.
 *
 * Finding (WAMR 2.4.5): the call does not return. The AOT compiler emits the suspend-flag check
 * at loop back-edges only for modules whose memory is *shared* (aot_emit_control.c,
 * check_suspend_flags: "Only need to check the suspend flags when memory is shared"); a block's
 * ordinary memory gets no check, so an endless loop in AOT code cannot be stopped. The test
 * confirms that limitation (exit 0) and fails as a tripwire if a WAMR release stops the loop, so
 * doc/blocks-wasm.md gets revisited.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "wasm_export.h"

namespace {

uint64_t now_us()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

double hc_read(wasm_exec_env_t, int32_t)
{
	return 0.0;
}

int64_t hc_now(wasm_exec_env_t)
{
	return 0;
}

int32_t hc_quality(wasm_exec_env_t, int32_t)
{
	return 0;
}

double hc_param(wasm_exec_env_t, int32_t)
{
	return 0.0;
}

void hc_write(wasm_exec_env_t, int32_t, double)
{
}

void hc_log(wasm_exec_env_t, const char *, int32_t)
{
}

NativeSymbol g_natives[] = {
	{"now", reinterpret_cast<void *>(hc_now), "()I", nullptr},
	{"read", reinterpret_cast<void *>(hc_read), "(i)F", nullptr},
	{"quality", reinterpret_cast<void *>(hc_quality), "(i)i", nullptr},
	{"param", reinterpret_cast<void *>(hc_param), "(i)F", nullptr},
	{"write", reinterpret_cast<void *>(hc_write), "(iF)", nullptr},
	{"log", reinterpret_cast<void *>(hc_log), "(*~)", nullptr},
};

wasm_module_inst_t g_instance;
wasm_exec_env_t g_env;
wasm_function_inst_t g_loop;
volatile bool g_returned;

void *worker(void *)
{
	wasm_runtime_init_thread_env();
	uint32_t args[1] = {0};
	wasm_runtime_call_wasm(g_env, g_loop, 0, args);
	g_returned = true;
	return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	std::string path = std::string(argv[1]) + "/wasm/probes.mt.x86_64.aot";
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		printf("cannot read %s\n", path.c_str());
		return 2;
	}
	std::vector<uint8_t> bytes;
	uint8_t buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
		bytes.insert(bytes.end(), buffer, buffer + n);
	fclose(f);

	if (!wasm_runtime_init())
		return 2;
	wasm_runtime_register_natives("hc", g_natives, sizeof g_natives / sizeof g_natives[0]);
	char error[128];
	wasm_module_t module = wasm_runtime_load(bytes.data(), static_cast<uint32_t>(bytes.size()), error, sizeof error);
	if (!module) {
		printf("load: %s\n", error);
		return 1;
	}
	g_instance = wasm_runtime_instantiate(module, 8192, 0, error, sizeof error);
	if (!g_instance) {
		printf("instantiate: %s\n", error);
		return 1;
	}
	g_env = wasm_runtime_create_exec_env(g_instance, 8192);
	g_loop = wasm_runtime_lookup_function(g_instance, "loop_forever");

	pthread_t thread;
	pthread_create(&thread, nullptr, worker, nullptr);
	usleep(20000);
	wasm_runtime_terminate(g_instance);
	uint64_t t0 = now_us();
	while (!g_returned && now_us() - t0 < 500000)
		usleep(1000);
	if (g_returned) {
		const char *exception = wasm_runtime_get_exception(g_instance);
		printf("FAIL (tripwire) WAMR now stops an AOT loop: \"%s\" after %llu us; revisit doc/blocks-wasm.md\n",
				exception ? exception : "", static_cast<unsigned long long>(now_us() - t0));
		return 1;
	}
	printf("PASS limitation confirmed: wasm_runtime_terminate() does not stop an AOT loop of a module without shared "
			"memory (still running 500 ms later)\n");
	fflush(stdout);
	_exit(0); /* the worker spins for ever */
}
