/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_FAKE_NAME_SERVICE_H
#define HC_FAKE_NAME_SERVICE_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "hc/platform/name_service.h"
#include "test_support.h"

namespace hc_test {

/// The LAN as mDNS sees it: the names other hosts answer for, the responder's renames while probing.
class FakeNameService : public hc::NameService {
public:
	explicit FakeNameService(const FakeClock& clock) : clock_(clock) {}

	bool start_lookup(const char* name, uint32_t timeout_ms, const char** reason) override {
		if (!lookups_start) {
			*reason = "mDNS is not running";
			return false;
		}
		if (running_) {
			*reason = "a lookup is still running";
			return false;
		}
		lookups.push_back(name);
		timeouts.push_back(timeout_ms);
		running_ = true;
		name_ = name;
		started_ms_ = clock_.monotonic_ms();
		return true;
	}

	hc::NameLookup poll_lookup() override {
		if (!running_) {
			return hc::NameLookup::failed;
		}
		if (hang || clock_.monotonic_ms() - started_ms_ < answer_ms) {
			return hc::NameLookup::running;
		}
		running_ = false;
		if (fail) {
			return hc::NameLookup::failed;
		}
		return taken.count(name_) != 0 ? hc::NameLookup::taken : hc::NameLookup::free;
	}

	void cancel_lookup() override {
		running_ = false;
		++cancels;
	}

	bool apply_name(const char* name, const char** reason) override {
		if (!apply_ok) {
			*reason = "mdns_init failed";
			return false;
		}
		applied.push_back(name);
		auto rename = probe_renames.find(name);
		announced = rename != probe_renames.end() ? rename->second : name;
		return true;
	}

	const char* announced_name() const override { return announced.c_str(); }

	std::set<std::string> taken;
	/// apply_name(key) ends up announcing the value (another host defended the name while probing).
	std::map<std::string, std::string> probe_renames;
	bool lookups_start = true;
	bool hang = false;
	bool fail = false;
	bool apply_ok = true;
	uint64_t answer_ms = 40;
	std::vector<std::string> lookups;
	std::vector<uint32_t> timeouts;
	std::vector<std::string> applied;
	std::string announced;
	int cancels = 0;

private:
	const FakeClock& clock_;
	bool running_ = false;
	std::string name_;
	uint64_t started_ms_ = 0;
};

} // namespace hc_test

#endif // HC_FAKE_NAME_SERVICE_H
