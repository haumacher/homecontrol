/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The host's facts for the System page (issue #42): from /proc and /sys, read per request.

#include "hc/posix/platform.h"

#include <dirent.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc {
namespace posix {

namespace {

/// Reads a small text file into `buffer` (NUL-terminated); false if it cannot be read.
bool read_file(const char* path, char* buffer, size_t size) {
	FILE* file = std::fopen(path, "r");
	if (file == nullptr) {
		return false;
	}
	size_t length = std::fread(buffer, 1, size - 1, file);
	std::fclose(file);
	buffer[length] = '\0';
	return length > 0;
}

TaskState map_state(char state) {
	switch (state) {
	case 'R': return TaskState::running;
	case 'S':
	case 'D':
	case 'I': return TaskState::blocked;
	case 'T':
	case 't': return TaskState::suspended;
	case 'Z':
	case 'X': return TaskState::deleted;
	default: return TaskState::unknown;
	}
}

/// The fields of /proc/<pid>/task/<tid>/stat the page uses.
struct ThreadStat {
	char name[32] = {};
	char state = '?';
	unsigned long long ticks = 0;      ///< utime + stime
	unsigned long long start = 0;      ///< starttime, ticks after boot
	int processor = -1;
};

bool parse_stat(const char* text, ThreadStat& out) {
	const char* open = std::strchr(text, '(');
	const char* close = std::strrchr(text, ')');
	if (open == nullptr || close == nullptr || close < open) {
		return false;
	}
	size_t length = static_cast<size_t>(close - open - 1);
	if (length >= sizeof(out.name)) {
		length = sizeof(out.name) - 1;
	}
	std::memcpy(out.name, open + 1, length);
	out.name[length] = '\0';
	// Field 3 (state) follows ") "; count fields from there.
	const char* p = close + 1;
	int field = 2;
	while (*p != '\0') {
		while (*p == ' ') {
			++p;
		}
		if (*p == '\0') {
			break;
		}
		++field;
		char* end = nullptr;
		switch (field) {
		case 3: out.state = *p; break;
		case 14: out.ticks += std::strtoull(p, &end, 10); break;
		case 15: out.ticks += std::strtoull(p, &end, 10); break;
		case 22: out.start = std::strtoull(p, &end, 10); break;
		case 39: out.processor = static_cast<int>(std::strtol(p, &end, 10)); break;
		default: break;
		}
		while (*p != '\0' && *p != ' ') {
			++p;
		}
	}
	return field >= 22;
}

} // namespace

MemoryInfo PosixSystemInfo::memory() const {
	MemoryInfo info{0, 0};
	char text[128];
	if (read_file("/proc/self/statm", text, sizeof(text))) {
		unsigned long long size = 0;
		unsigned long long resident = 0;
		if (std::sscanf(text, "%llu %llu", &size, &resident) == 2) {
			info.resident_bytes = static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
		}
	}
	return info;
}

bool PosixSystemInfo::visit_tasks(TaskVisitor& visitor, const char** reason) const {
	DIR* dir = opendir("/proc/self/task");
	if (dir == nullptr) {
		*reason = "cannot read /proc/self/task";
		return false;
	}
	long hz = sysconf(_SC_CLK_TCK);
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	double uptime_s = 0;
	char text[512];
	if (read_file("/proc/uptime", text, sizeof(text))) {
		uptime_s = std::strtod(text, nullptr);
	}
	long self = static_cast<long>(syscall(SYS_gettid));
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		long tid = std::strtol(entry->d_name, nullptr, 10);
		char path[64];
		std::snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
		ThreadStat stat;
		if (!read_file(path, text, sizeof(text)) || !parse_stat(text, stat)) {
			continue; // the thread ended meanwhile
		}
		TaskInfo info;
		info.name = stat.name;
		info.state = map_state(stat.state);
		info.current = tid == self;
		if (hz > 0 && cores > 0 && uptime_s > 0) {
			double alive = uptime_s - static_cast<double>(stat.start) / static_cast<double>(hz);
			if (alive > 0) {
				double share = static_cast<double>(stat.ticks) / static_cast<double>(hz) / alive / static_cast<double>(cores);
				info.cpu_permille = static_cast<int32_t>(share * 1000.0 + 0.5);
			} else {
				info.cpu_permille = 0; // younger than a clock tick (started after /proc/uptime was read): nothing measurable yet
			}
		}
		visitor.task(info);
	}
	closedir(dir);
	return true;
}

bool PosixSystemInfo::temperature(Temperature& out, const char** reason) const {
	char text[64];
	if (!read_file("/sys/class/thermal/thermal_zone0/temp", text, sizeof(text))) {
		*reason = "the host has no /sys/class/thermal/thermal_zone0";
		return false;
	}
	out.celsius = std::strtod(text, nullptr) / 1000.0;
	out.accuracy = 0;
	out.source = "thermal_zone0";
	return true;
}

void PosixSystemInfo::visit_chip(DiagnosticsVisitor& visitor) const {
	char text[4096];
	if (read_file("/proc/cpuinfo", text, sizeof(text))) {
		const char* line = std::strstr(text, "model name");
		const char* colon = line != nullptr ? std::strchr(line, ':') : nullptr;
		if (colon != nullptr) {
			char model[96];
			const char* start = colon + 1;
			while (*start == ' ' || *start == '\t') {
				++start;
			}
			size_t length = std::strcspn(start, "\n");
			if (length >= sizeof(model)) {
				length = sizeof(model) - 1;
			}
			std::memcpy(model, start, length);
			model[length] = '\0';
			visitor.text("model", model);
		}
	}
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (cores > 0) {
		visitor.number("cores", static_cast<uint64_t>(cores));
	}
	utsname name;
	if (uname(&name) == 0) {
		char os[160];
		std::snprintf(os, sizeof(os), "%s %s", name.sysname, name.release);
		visitor.text("arch", name.machine);
		visitor.text("os", os);
	}
}

bool PosixSystemInfo::visit_settings_store(DiagnosticsVisitor&, const char** reason) const {
	*reason = "the host keeps its settings in a text file (no NVS)";
	return false;
}

} // namespace posix
} // namespace hc
