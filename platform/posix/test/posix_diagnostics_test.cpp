/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The POSIX diagnostics (issue #4, doc/diagnostics.md): the run marker and the reset reason it gives.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "doctest.h"
#include "hc/diagnostics.h"
#include "hc/posix/diagnostics.h"
#include "test_support.h"

using hc::ResetReason;
using hc::posix::PosixDiagnostics;

namespace {

/// A fresh marker path in the temporary directory, removed afterwards.
struct TempMarker {
	TempMarker() {
		char pattern[] = "/tmp/hc_diag_XXXXXX";
		int fd = mkstemp(pattern);
		REQUIRE(fd >= 0);
		close(fd);
		path = pattern;
		std::remove(path.c_str());
	}
	~TempMarker() { std::remove(path.c_str()); }

	void write(const char* text) const {
		std::FILE* file = std::fopen(path.c_str(), "w");
		REQUIRE(file != nullptr);
		std::fputs(text, file);
		std::fclose(file);
	}

	std::string read() const {
		std::string out;
		std::FILE* file = std::fopen(path.c_str(), "r");
		if (file == nullptr) {
			return out;
		}
		char buffer[256];
		size_t n = 0;
		while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
			out.append(buffer, n);
		}
		std::fclose(file);
		return out;
	}

	std::string path;
};

struct Facts : hc::DiagnosticsVisitor {
	void number(const char* key, uint64_t value) override { values[key] = std::to_string(value); }
	void text(const char* key, const char* value) override { values[key] = value; }
	void flag(const char* key, bool value) override { values[key] = value ? "true" : "false"; }
	std::map<std::string, std::string> values;
};

Facts facts_of(const PosixDiagnostics& diagnostics) {
	Facts facts;
	diagnostics.visit_boot(facts);
	return facts;
}

} // namespace

TEST_CASE("PosixDiagnostics.MarkerBesideTheSettings") {
	CHECK(PosixDiagnostics::marker_for_settings("homecontrol.settings") == "homecontrol.run");
	CHECK(PosixDiagnostics::marker_for_settings("/etc/hc/home.settings") == "/etc/hc/home.run");
	CHECK(PosixDiagnostics::marker_for_settings("config.txt") == "config.txt.run");
}

TEST_CASE("PosixDiagnostics.FirstStartIsPowerOnWithPidAndStartTime") {
	TempMarker marker;
	PosixDiagnostics diagnostics(marker.path);
	diagnostics.start();
	CHECK(diagnostics.reset_reason() == ResetReason::power_on);
	Facts facts = facts_of(diagnostics);
	CHECK(facts.values["pid"] == std::to_string(getpid()));
	CHECK(std::strtoll(facts.values["started"].c_str(), nullptr, 10) > 1700000000);
	CHECK(facts.values.count("previous_signal") == 0);
	CHECK(marker.read().rfind("pid " + std::to_string(getpid()) + " started ", 0) == 0);
}

TEST_CASE("PosixDiagnostics.StopSignalIsReportedAtTheNextStart") {
	TempMarker marker;
	{
		PosixDiagnostics run(marker.path);
		run.start();
		run.stopped(SIGTERM);
	}
	PosixDiagnostics next(marker.path);
	next.start();
	CHECK(next.reset_reason() == ResetReason::software);
	CHECK(facts_of(next).values["previous_signal"] == "SIGTERM");
	// The marker now describes the new run only.
	CHECK(marker.read().find("signal") == std::string::npos);
}

TEST_CASE("PosixDiagnostics.MarkerWithoutSignalIsUnknown") {
	TempMarker marker;
	marker.write("pid 4242 started 1760000000\n");
	PosixDiagnostics diagnostics(marker.path);
	diagnostics.start();
	CHECK(diagnostics.reset_reason() == ResetReason::unknown);
	CHECK(facts_of(diagnostics).values["previous_signal"] == "none recorded");
}

TEST_CASE("PosixDiagnostics.FatalSignalIsRecordedByTheHandler") {
	TempMarker marker;
	pid_t child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		PosixDiagnostics run(marker.path);
		run.start();
		raise(SIGABRT);
		_exit(0); // not reached
	}
	int status = 0;
	REQUIRE(waitpid(child, &status, 0) == child);
	CHECK(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
	CHECK(marker.read().find("signal 6\n") != std::string::npos);
	PosixDiagnostics next(marker.path);
	next.start();
	CHECK(next.reset_reason() == ResetReason::panic);
	CHECK(facts_of(next).values["previous_signal"] == "SIGABRT");
}

TEST_CASE("PosixDiagnostics.UnwritableMarkerIsLogged") {
	hc_test::ScopedCapture capture;
	PosixDiagnostics diagnostics("/nonexistent-dir/hc.run");
	diagnostics.start();
	CHECK(diagnostics.reset_reason() == ResetReason::power_on);
	CHECK(capture.sink.contains("cannot write the run marker /nonexistent-dir/hc.run"));
}
