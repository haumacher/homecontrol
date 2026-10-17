/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/diagnostics.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "hc/log.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.posix.diag";

/// The marker's path for the signal handler (no std::string there). Longer paths get no crash
/// record (logged at start).
char g_marker[512];

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

/// Appends "signal <n>\n" to the marker with async-signal-safe calls only.
void append_signal(int signal) {
	if (g_marker[0] == '\0') {
		return;
	}
	char text[24] = "signal ";
	size_t length = 7;
	char digits[12];
	size_t count = 0;
	unsigned value = static_cast<unsigned>(signal);
	do {
		digits[count++] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0 && count < sizeof(digits));
	while (count > 0) {
		text[length++] = digits[--count];
	}
	text[length++] = '\n';
	int fd = open(g_marker, O_WRONLY | O_APPEND);
	if (fd >= 0) {
		ssize_t written = write(fd, text, length);
		(void)written;
		close(fd);
	}
}

void on_fatal_signal(int signal) {
	append_signal(signal);
	raise(signal); // SA_RESETHAND restored the default action: the signal now ends the process

}

} // namespace

PosixDiagnostics::PosixDiagnostics(std::string marker_path) : path_(std::move(marker_path)) {}

std::string PosixDiagnostics::marker_for_settings(const std::string& settings_path) {
	static const std::string kSettings = ".settings";
	std::string base = settings_path;
	size_t slash = base.find_last_of('/');
	size_t stem = slash == std::string::npos ? 0 : slash + 1;
	if (base.size() > stem + kSettings.size() &&
		base.compare(base.size() - kSettings.size(), kSettings.size(), kSettings) == 0) {
		base.resize(base.size() - kSettings.size());
	}
	return base + ".run";
}

std::string PosixDiagnostics::signal_name(int signal) {
	switch (signal) {
	case SIGHUP: return "SIGHUP";
	case SIGINT: return "SIGINT";
	case SIGTERM: return "SIGTERM";
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS: return "SIGBUS";
	case SIGFPE: return "SIGFPE";
	case SIGILL: return "SIGILL";
	case SIGABRT: return "SIGABRT";
	default: return "signal " + std::to_string(signal);
	}
}

void PosixDiagnostics::start() {
	pid_ = static_cast<long>(getpid());
	started_ = static_cast<int64_t>(std::time(nullptr));

	std::FILE* previous = std::fopen(path_.c_str(), "r");
	if (previous == nullptr) {
		reason_ = ResetReason::power_on;
		previous_signal_ = -1;
	} else {
		previous_signal_ = 0;
		char line[128];
		while (std::fgets(line, sizeof(line), previous) != nullptr) {
			int signal = 0;
			if (std::sscanf(line, "signal %d", &signal) == 1 && signal > 0) {
				previous_signal_ = signal; // the last one counts
			}
		}
		std::fclose(previous);
		if (previous_signal_ == 0) {
			reason_ = ResetReason::unknown;
		} else if (previous_signal_ == SIGINT || previous_signal_ == SIGTERM || previous_signal_ == SIGHUP) {
			reason_ = ResetReason::software;
		} else {
			reason_ = ResetReason::panic;
		}
	}

	std::FILE* marker = std::fopen(path_.c_str(), "w");
	if (marker == nullptr) {
		Log::warn(kTag, "cannot write the run marker %s (%s): the next start cannot tell how this run ended",
			path_.c_str(), std::strerror(errno));
		g_marker[0] = '\0';
		return;
	}
	std::fprintf(marker, "pid %ld started %lld\n", pid_, static_cast<long long>(started_));
	std::fclose(marker);
	if (path_.size() >= sizeof(g_marker)) {
		Log::warn(kTag, "run marker path longer than %u characters: a crash is not recorded",
			static_cast<unsigned>(sizeof(g_marker) - 1));
		g_marker[0] = '\0';
		return;
	}
	std::memcpy(g_marker, path_.c_str(), path_.size() + 1);
	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = on_fatal_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESETHAND;
	for (int signal : kFatalSignals) {
		sigaction(signal, &action, nullptr);
	}
}

void PosixDiagnostics::stopped(int signal) {
	append_signal(signal);
}

void PosixDiagnostics::visit_boot(DiagnosticsVisitor& visitor) const {
	visitor.number("pid", static_cast<uint64_t>(pid_));
	visitor.number("started", static_cast<uint64_t>(started_));
	if (previous_signal_ > 0) {
		visitor.text("previous_signal", signal_name(previous_signal_).c_str());
	} else if (previous_signal_ == 0) {
		visitor.text("previous_signal", "none recorded");
	}
}

} // namespace posix
} // namespace hc
