/*
#include <cstring>
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the external values (issue #17): two sources from one JSON
// configuration, all-or-nothing extraction, the stale rule with recovery, a refused reconfiguration
// that keeps the old one, and commands refused.

#include <string>
#include <vector>

#include "doctest.h"
#include "fake_fetch.h"
#include "hc/external_values.h"
#include "hc/time_service.h"
#include "test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::ExternalValues;
using hc::Quality;
using hc::Value;
using hc::ValueType;

namespace {

class Fetcher : public hc::HttpFetcher {
public:
	explicit Fetcher(const hc_test::FakeClock& clock) : clock_(clock) {}
	bool fetch(const char* url, hc::FetchReceiver& receiver, const char** error) override {
		if (receiver_ != nullptr) {
			*error = "busy";
			return false;
		}
		urls.push_back(url);
		receiver_ = &receiver;
		return true;
	}
	bool busy() const override { return receiver_ != nullptr; }
	bool pending() const { return receiver_ != nullptr; }
	void ok(const std::string& body) {
		hc::FetchReceiver* receiver = receiver_;
		receiver_ = nullptr;
		hc_test::respond(*receiver, 200, body);
	}
	void fail(int status) {
		hc::FetchReceiver* receiver = receiver_;
		receiver_ = nullptr;
		hc_test::respond(*receiver, status, "probe failure");
	}
	std::vector<std::string> urls;

private:
	const hc_test::FakeClock& clock_;
	hc::FetchReceiver* receiver_ = nullptr;
};

struct Rig {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	hc_test::FakeTimeZone zone;
	hc_test::FakeSettings settings;
	hc::TimeService time{loop, clock, zone, settings};
	hc::DatapointModel model{clock};
	Fetcher fetcher{clock};
	ExternalValues ext{loop, time, settings, connectivity, model, fetcher};
	Rig() {
		clock.wall_seconds = 1792270800;
		clock.synchronized = true;
		settings.set_string(hc::setting::kLatitude, "48.1");
		settings.set_string(hc::setting::kLongitude, "11.6");
		connectivity.start();
		connectivity.link_up("192.0.2.100");
		DatapointSpec specs[] = {
			{"out.temp", ValueType::real, DatapointFlags::kReadable},
			{"pv.power", ValueType::real, DatapointFlags::kReadable},
			{"pv.online", ValueType::boolean, DatapointFlags::kReadable},
		};
		REQUIRE(model.load(specs, 3) == DatapointStatus::ok);
	}
	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}
	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }
	const hc::Datapoint& dp(const char* name) {
		const hc::Datapoint* d = model.find(name);
		REQUIRE(d != nullptr);
		return *d;
	}
	/// Runs until the fetcher has a request pending (or `max_ms` passed).
	bool wait_request(uint64_t max_ms) {
		uint64_t end = clock.now_ms + max_ms;
		while (clock.now_ms < end && !fetcher.pending()) {
			uint64_t deadline = loop.next_deadline();
			clock.now_ms = deadline > clock.now_ms && deadline < end ? deadline : clock.now_ms + 1000;
			loop.run_once(clock.now_ms);
		}
		return fetcher.pending();
	}
};

const char* kConfig = R"({"sources": [
 {"id": "weather", "preset": "open-meteo", "interval": 900,
  "bindings": [{"datapoint": "out.temp", "variable": "temperature"}]},
 {"id": "pv", "url": "http://pv.example/status.json", "interval": 300,
  "bindings": [{"datapoint": "pv.power", "path": "inverters[0].ac.power", "scale": 0.001, "unit": "kW"},
               {"datapoint": "pv.online", "path": "online"}]}
]})";

} // namespace

TEST_CASE("ExternalProbe.TwoSourcesAllOrNothingStaleAndRecovery") {
	Rig r;
	const char* error = nullptr;
	REQUIRE(r.ext.configure_json(kConfig, std::strlen(kConfig), &error));
	CHECK(r.ext.size() == 2);
	// First the weather (source 0), then pv (source 1), spread apart.
	REQUIRE(r.wait_request(60000));
	CHECK(r.fetcher.urls.back().rfind("https://api.open-meteo.com/", 0) == 0);
	CHECK(r.fetcher.urls.back().find("latitude=48.1") != std::string::npos);
	r.fetcher.ok(R"({"current": {"temperature_2m": 12.5}, "hourly": {"temperature_2m": [12.5]}})");
	CHECK(r.dp("out.temp").value().as_real() == doctest::Approx(12.5));
	CHECK(r.dp("out.temp").quality() == Quality::good);
	REQUIRE(r.wait_request(60000));
	CHECK(r.fetcher.urls.back() == "http://pv.example/status.json");
	// A response with one path missing: nothing of this source is reported.
	r.fetcher.ok(R"({"inverters": [{"ac": {"power": 1234}}]})");
	CHECK(r.dp("pv.power").quality() == Quality::unknown);
	CHECK(r.dp("pv.online").quality() == Quality::unknown);
	CHECK(r.capture.sink.count("online") >= 1); // the missing path is named
	// The retry (backoff) brings a complete response: both reported, scaled.
	REQUIRE(r.wait_request(2 * 60 * 1000 + 5000));
	CHECK(r.fetcher.urls.back() == "http://pv.example/status.json");
	r.fetcher.ok(R"({"inverters": [{"ac": {"power": 1234}}], "online": true})");
	CHECK(r.dp("pv.power").value().as_real() == doctest::Approx(1.234));
	CHECK(r.dp("pv.online").value() == Value::of_bool(true));
	CHECK(r.dp("pv.online").quality() == Quality::good);
	// Two failures in a row: stale (values kept); a success recovers.
	size_t stale_logs = r.capture.sink.count("stale");
	for (int i = 0; i < 2; ++i) {
		REQUIRE(r.wait_request(70 * 60 * 1000));
		if (r.fetcher.urls.back().rfind("https://api.open-meteo.com/", 0) == 0) {
			r.fetcher.ok(R"({"current": {"temperature_2m": 12.0}, "hourly": {"temperature_2m": [12.0]}})");
			--i;
			continue;
		}
		r.fetcher.fail(503);
	}
	CHECK(r.dp("pv.power").quality() == Quality::stale);
	CHECK(r.dp("pv.power").value().as_real() == doctest::Approx(1.234));
	CHECK(r.capture.sink.count("stale") > stale_logs);
	for (;;) {
		REQUIRE(r.wait_request(70 * 60 * 1000));
		if (r.fetcher.urls.back() == "http://pv.example/status.json") {
			r.fetcher.ok(R"({"inverters": [{"ac": {"power": 500}}], "online": false})");
			break;
		}
		r.fetcher.ok(R"({"current": {"temperature_2m": 11.0}, "hourly": {"temperature_2m": [11.0]}})");
	}
	CHECK(r.dp("pv.power").quality() == Quality::good);
	CHECK(r.dp("pv.power").value().as_real() == doctest::Approx(0.5));
	CHECK(r.capture.sink.count("recovered") >= 1);
	// Commands are refused: these values come from outside.
	CHECK(r.model.command(1, Value::of_real(1.0)) != DatapointStatus::ok);
	// A refused reconfiguration (unknown member) keeps the two sources.
	const char* bad = R"({"sources": [{"id": "x", "url": "http://h/x", "bindings": [], "colour": 1}]})";
	CHECK_FALSE(r.ext.configure_json(bad, std::strlen(bad), &error));
	CHECK(error != nullptr);
	CHECK(r.ext.size() == 2);
}
