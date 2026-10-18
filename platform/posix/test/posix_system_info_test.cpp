/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The host's facts for the System page (issue #42): the process's threads, its resident memory, the
// CPU, and honest refusals for what Linux does not have (heap budget, NVS).

#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "hc/posix/platform.h"

namespace {

struct Tasks : hc::TaskVisitor {
	void task(const hc::TaskInfo& info) override {
		names.push_back(info.name);
		if (info.current) {
			++current;
			current_state = info.state;
		}
		if (info.cpu_permille < 0 || info.cpu_permille > 1000) {
			++bad_cpu;
		}
		if (info.stack_free >= 0 || info.priority >= 0) {
			++unexpected;
		}
	}
	std::vector<std::string> names;
	int current = 0;
	hc::TaskState current_state = hc::TaskState::unknown;
	int bad_cpu = 0;
	int unexpected = 0;
};

struct Facts : hc::DiagnosticsVisitor {
	void number(const char* key, uint64_t) override { keys.push_back(key); }
	void text(const char* key, const char*) override { keys.push_back(key); }
	void flag(const char* key, bool) override { keys.push_back(key); }
	bool has(const std::string& key) const {
		for (const std::string& k : keys) {
			if (k == key) {
				return true;
			}
		}
		return false;
	}
	std::vector<std::string> keys;
};

} // namespace

TEST_CASE("PosixSystemInfo: the threads of the process, the calling one marked") {
	hc::posix::PosixSystemInfo info;
	Tasks tasks;
	const char* reason = nullptr;
	REQUIRE(info.visit_tasks(tasks, &reason));
	CHECK(tasks.names.size() >= 1);
	CHECK(tasks.current == 1);
	CHECK(tasks.current_state == hc::TaskState::running);
	CHECK(tasks.bad_cpu == 0);
	CHECK(tasks.unexpected == 0); // no stack or priority figures on the host

	// A second thread appears while it runs.
	size_t before = tasks.names.size();
	bool release = false;
	std::thread other([&release]() {
		while (!__atomic_load_n(&release, __ATOMIC_ACQUIRE)) {
			std::this_thread::yield();
		}
	});
	Tasks more;
	REQUIRE(info.visit_tasks(more, &reason));
	__atomic_store_n(&release, true, __ATOMIC_RELEASE);
	other.join();
	CHECK(more.names.size() == before + 1);
}

TEST_CASE("PosixSystemInfo: resident memory instead of a heap budget, the CPU, no NVS") {
	hc::posix::PosixSystemInfo info;
	hc::MemoryInfo memory = info.memory();
	CHECK(memory.free_bytes == 0);
	CHECK(memory.resident_bytes > 0);

	Facts chip;
	info.visit_chip(chip);
	CHECK(chip.has("cores"));
	CHECK(chip.has("os"));

	Facts nvs;
	const char* reason = nullptr;
	CHECK_FALSE(info.visit_settings_store(nvs, &reason));
	CHECK(std::string(reason).find("text file") != std::string::npos);

	// The thermal zone exists on some hosts only: either a plausible reading or the reason.
	hc::Temperature temperature;
	reason = nullptr;
	if (info.temperature(temperature, &reason)) {
		CHECK(temperature.celsius > -40);
		CHECK(temperature.celsius < 150);
	} else {
		CHECK(std::string(reason).find("thermal_zone0") != std::string::npos);
	}
}
