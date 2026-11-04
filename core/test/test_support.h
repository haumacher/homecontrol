/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_SUPPORT_H
#define HC_TEST_SUPPORT_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hc/controller.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/improv_port.h"
#include "hc/platform/network_driver.h"
#include "hc/platform/settings.h"
#include "hc/platform/status_light.h"
#include "hc/platform/system_info.h"
#include "hc/platform/time_zone.h"
#include "hc/platform/wifi_scanner.h"
#include "hc/tz.h"

namespace hc_test {

/// A clock the test advances by hand. Never reads the real time.
class FakeClock : public hc::Clock {
public:
	uint64_t monotonic_ms() const override { return now_ms; }
	hc::WallTime wall_time() const override { return hc::WallTime{wall_seconds, synchronized}; }

	void advance(uint64_t ms) { now_ms += ms; }

	uint64_t now_ms = 1000;
	int64_t wall_seconds = 0;
	bool synchronized = false;
};

/// Fixed memory figures; tasks, a temperature and chip facts when a test sets them (the System page).
class FakeSystemInfo : public hc::SystemInfo {
public:
	hc::MemoryInfo memory() const override {
		hc::MemoryInfo info{free_bytes, min_free_bytes};
		info.largest_block = largest_block;
		return info;
	}

	bool visit_tasks(hc::TaskVisitor& visitor, const char** reason) const override {
		if (!list_tasks) {
			return hc::SystemInfo::visit_tasks(visitor, reason);
		}
		for (const hc::TaskInfo& task : tasks) {
			visitor.task(task);
		}
		return true;
	}

	bool temperature(hc::Temperature& out, const char** reason) const override {
		if (!has_temperature) {
			return hc::SystemInfo::temperature(out, reason);
		}
		++temperature_reads;
		out = reading;
		return true;
	}

	void visit_chip(hc::DiagnosticsVisitor& visitor) const override {
		if (chip) {
			visitor.text("model", "ESP32-C6");
			visitor.number("cpu_mhz", 160);
		}
	}

	bool console_output(hc::ConsoleOutputInfo& out, const char** reason) const override {
		if (!has_console) {
			return hc::SystemInfo::console_output(out, reason);
		}
		out = console;
		return true;
	}

	uint32_t free_bytes = 300000;
	uint32_t min_free_bytes = 250000;
	uint32_t largest_block = 0;
	bool list_tasks = false;
	std::vector<hc::TaskInfo> tasks;
	bool has_temperature = false;
	hc::Temperature reading;
	mutable int temperature_reads = 0;
	bool chip = false;
	bool has_console = false;
	hc::ConsoleOutputInfo console;
};

/// Records every message as "<letter> <tag>: <message>".
class CapturingSink : public hc::LogSink {
public:
	void write(hc::LogLevel level, const char* tag, const char* message) override {
		lines.push_back(std::string(hc::log_level_letter(level)) + " " + tag + ": " + message);
	}

	/// Number of recorded lines containing `text`.
	size_t count(const std::string& text) const {
		size_t result = 0;
		for (const std::string& line : lines) {
			if (line.find(text) != std::string::npos) {
				++result;
			}
		}
		return result;
	}

	bool contains(const std::string& text) const { return count(text) > 0; }

	std::vector<std::string> lines;
};

/// Installs a CapturingSink for the scope of a test and restores the previous sink and level.
class ScopedCapture {
public:
	explicit ScopedCapture(hc::LogLevel level = hc::LogLevel::info)
		: previous_sink_(hc::Log::sink()), previous_level_(hc::Log::level()) {
		hc::Log::set_sink(&sink);
		hc::Log::set_level(level);
	}
	~ScopedCapture() {
		hc::Log::set_sink(previous_sink_);
		hc::Log::set_level(previous_level_);
	}

	CapturingSink sink;

private:
	hc::LogSink* previous_sink_;
	hc::LogLevel previous_level_;
};

/// Settings in memory, with the validation rules every platform implementation applies.
class FakeSettings : public hc::Settings {
public:
	struct Entry {
		hc::SettingType type;
		std::string text;
		int32_t number;
	};

	hc::SettingsStatus get_string(const char* key, char* buffer, size_t size) const override {
		if (size > 0) {
			buffer[0] = '\0';
		}
		const Entry* entry = nullptr;
		hc::SettingsStatus status = find(key, hc::SettingType::string, entry);
		if (status != hc::SettingsStatus::ok) {
			return status;
		}
		if (entry->text.size() + 1 > size) {
			return hc::SettingsStatus::too_long;
		}
		std::memcpy(buffer, entry->text.c_str(), entry->text.size() + 1);
		return hc::SettingsStatus::ok;
	}

	hc::SettingsStatus set_string(const char* key, const char* value) override {
		hc::SettingsStatus status = check_key(key);
		if (status == hc::SettingsStatus::ok) {
			status = check_string(value);
		}
		if (status != hc::SettingsStatus::ok) {
			return status;
		}
		if (fail_writes) {
			return hc::SettingsStatus::storage_error;
		}
		entries[key] = Entry{hc::SettingType::string, value, 0};
		++writes;
		return hc::SettingsStatus::ok;
	}

	hc::SettingsStatus get_int(const char* key, int32_t& value) const override {
		const Entry* entry = nullptr;
		hc::SettingsStatus status = find(key, hc::SettingType::integer, entry);
		if (status == hc::SettingsStatus::ok) {
			value = entry->number;
		}
		return status;
	}

	hc::SettingsStatus set_int(const char* key, int32_t value) override {
		return set_number(key, hc::SettingType::integer, value);
	}

	hc::SettingsStatus get_bool(const char* key, bool& value) const override {
		const Entry* entry = nullptr;
		hc::SettingsStatus status = find(key, hc::SettingType::boolean, entry);
		if (status == hc::SettingsStatus::ok) {
			value = entry->number != 0;
		}
		return status;
	}

	hc::SettingsStatus set_bool(const char* key, bool value) override {
		return set_number(key, hc::SettingType::boolean, value ? 1 : 0);
	}

	hc::SettingsStatus erase(const char* key) override {
		hc::SettingsStatus status = check_key(key);
		if (status != hc::SettingsStatus::ok) {
			return status;
		}
		if (entries.count(key) == 0) {
			return hc::SettingsStatus::not_found; // nothing written (NVS writes nothing either)
		}
		if (fail_writes) {
			return hc::SettingsStatus::storage_error;
		}
		entries.erase(key);
		++writes;
		return hc::SettingsStatus::ok;
	}

	void visit(hc::SettingsVisitor& visitor) const override {
		for (const auto& entry : entries) {
			visitor.entry(entry.first.c_str(), entry.second.type);
		}
	}

	std::map<std::string, Entry> entries;
	bool fail_writes = false;
	/// Successful sets and erases (the flash writes a store would make).
	size_t writes = 0;

private:
	hc::SettingsStatus find(const char* key, hc::SettingType type, const Entry*& out) const {
		hc::SettingsStatus status = check_key(key);
		if (status != hc::SettingsStatus::ok) {
			return status;
		}
		auto it = entries.find(key);
		if (it == entries.end()) {
			return hc::SettingsStatus::not_found;
		}
		if (it->second.type != type) {
			return hc::SettingsStatus::wrong_type;
		}
		out = &it->second;
		return hc::SettingsStatus::ok;
	}

	hc::SettingsStatus set_number(const char* key, hc::SettingType type, int32_t value) {
		hc::SettingsStatus status = check_key(key);
		if (status != hc::SettingsStatus::ok) {
			return status;
		}
		if (fail_writes) {
			return hc::SettingsStatus::storage_error;
		}
		entries[key] = Entry{type, std::string(), value};
		++writes;
		return hc::SettingsStatus::ok;
	}
};

/// A time zone with a fixed offset, whatever TZ string is applied (the string is recorded).
class FakeTimeZone : public hc::TimeZone {
public:
	void apply(const char* posix_tz) override { applied.push_back(posix_tz); }

	bool to_local(int64_t utc_seconds, hc::LocalTime& out) const override {
		int64_t local = utc_seconds + offset_s;
		int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
		int64_t rest = local - days * 86400;
		// Inverse of hc::days_from_civil (Howard Hinnant's civil_from_days).
		int64_t z = days + 719468;
		int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		int64_t doe = z - era * 146097;
		int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		int64_t mp = (5 * doy + 2) / 153;
		int64_t day = doy - (153 * mp + 2) / 5 + 1;
		int64_t month = mp < 10 ? mp + 3 : mp - 9;
		int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
		out.year = static_cast<int32_t>(year);
		out.month = static_cast<uint8_t>(month);
		out.day = static_cast<uint8_t>(day);
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.dst = dst;
		std::snprintf(out.zone, sizeof(out.zone), "%s", zone.c_str());
		return true;
	}

	std::vector<std::string> applied;
	int32_t offset_s = 7200;
	bool dst = true;
	std::string zone = "CEST";
};

/// Records the driver calls with the time they happened; results are configurable. `calls` holds
/// "connect@<ms>", "disconnect@<ms>", "pair@<ms>", "unpair@<ms>" in order.
class FakeNetworkDriver : public hc::NetworkDriver {
public:
	explicit FakeNetworkDriver(const FakeClock& clock) : clock_(clock) {}

	bool provisioned() const override { return has_credentials; }

	void power_save_changed() override {
		++power_save_changes;
		record("power_save");
	}
	int power_save_changes = 0;

	hc::ConnectResult connect() override {
		connects.push_back(clock_.now_ms);
		record("connect");
		if (on_connect) {
			on_connect();
		}
		return result;
	}

	void disconnect() override {
		++disconnects;
		record("disconnect");
	}

	bool start_pairing() override {
		pairings.push_back(clock_.now_ms);
		record("pair");
		return pairing_starts;
	}

	void stop_pairing() override {
		++pairing_stops;
		record("unpair");
	}

	void record(const char* what) { calls.push_back(std::string(what) + "@" + std::to_string(clock_.now_ms)); }

	bool has_credentials = true;
	hc::ConnectResult result = hc::ConnectResult::started;
	bool pairing_starts = true;
	std::vector<uint64_t> connects;
	std::vector<uint64_t> pairings;
	std::vector<std::string> calls;
	int disconnects = 0;
	int pairing_stops = 0;
	std::function<void()> on_connect;

private:
	const FakeClock& clock_;
};

/// Records the levels the controller sets, with the time.
class FakeStatusLight : public hc::StatusLight {
public:
	explicit FakeStatusLight(const FakeClock& clock) : clock_(clock) {}

	void set(bool on) override { changes.emplace_back(clock_.now_ms, on); }

	std::vector<std::pair<uint64_t, bool>> changes;

private:
	const FakeClock& clock_;
};

/// Records the frames sent and whether Improv ended.
class FakeImprovPort : public hc::ImprovPort {
public:
	void send_frame(const uint8_t* frame, size_t length) override { frames.emplace_back(frame, frame + length); }
	void improv_ended() override { ++ended; }
	const char* chip_family() const override { return "ESP32-C6"; }
	const char* device_name() const override { return "homecontrol-a1b2c3"; }

	std::vector<std::vector<uint8_t>> frames;
	int ended = 0;
};

/// Records scan starts; the test delivers the results through `sink`.
class FakeScanner : public hc::WifiScanner {
public:
	bool start_scan(hc::WifiScanSink& sink_) override {
		++starts;
		sink = &sink_;
		return accept;
	}

	bool accept = true;
	int starts = 0;
	hc::WifiScanSink* sink = nullptr;
};

/// All fakes a Controller needs, wired into an hc::Platform.
struct FakePlatform {
	FakeClock clock;
	CapturingSink sink;
	FakeSystemInfo system;
	FakeSettings settings;
	FakeTimeZone time_zone;
	FakeNetworkDriver network{clock};

	hc::Platform platform() { return hc::Platform{clock, sink, system, settings, time_zone, network}; }
};

} // namespace hc_test

#endif // HC_TEST_SUPPORT_H
