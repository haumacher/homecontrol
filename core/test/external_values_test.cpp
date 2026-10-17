/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/external_values.h"
#include "test_support.h"

#ifndef HC_SPEC_DIR
#error "HC_SPEC_DIR must name the spec directory"
#endif

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::ExternalBindingSpec;
using hc::ExternalSourceSpec;
using hc::ExternalSourceStatus;
using hc::ExternalValues;
using hc::JsonSpan;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc_test::FakeClock;
using hc_test::FakeNetworkDriver;
using hc_test::FakeSettings;
using hc_test::FakeTimeZone;
using hc_test::ScopedCapture;

namespace {

std::string read_fixture(const char* name) {
	std::ifstream in(std::string(HC_SPEC_DIR) + "/fixtures/external/" + name, std::ios::binary);
	REQUIRE(in.good());
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

/// Records the fetches; the test completes them.
class FakeFetcher : public hc::HttpFetcher {
public:
	struct Request {
		std::string url;
		size_t max_bytes;
		uint64_t at;
	};

	explicit FakeFetcher(const FakeClock& clock) : clock_(clock) {}

	bool fetch(const char* url, size_t max_bytes, hc::FetchReceiver& receiver, const char** error) override {
		if (refuse != nullptr) {
			*error = refuse;
			return false;
		}
		if (receiver_ != nullptr) {
			*error = "busy: a fetch is in flight";
			return false;
		}
		requests.push_back(Request{url, max_bytes, clock_.now_ms});
		receiver_ = &receiver;
		return true;
	}

	bool busy() const override { return receiver_ != nullptr; }

	void ok(const std::string& body) {
		REQUIRE(receiver_ != nullptr);
		hc::FetchResult result;
		result.ok = true;
		result.http_status = 200;
		result.body = body.data();
		result.length = body.size();
		deliver(result);
	}

	void fail(int status, const char* error = "") {
		REQUIRE(receiver_ != nullptr);
		hc::FetchResult result;
		result.http_status = status;
		result.error = error;
		deliver(result);
	}

	void deliver(const hc::FetchResult& result) {
		hc::FetchReceiver* receiver = receiver_;
		receiver_ = nullptr;
		receiver->fetch_done(result);
	}

	std::vector<Request> requests;
	const char* refuse = nullptr;

private:
	const FakeClock& clock_;
	hc::FetchReceiver* receiver_ = nullptr;
};

constexpr uint8_t kR = DatapointFlags::kReadable;

struct Fixture {
	ScopedCapture capture{hc::LogLevel::debug};
	FakeClock clock;
	hc::EventLoop loop;
	FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	FakeTimeZone zone;
	FakeSettings settings;
	hc::TimeService time{loop, clock, zone, settings};
	hc::DatapointModel model{clock};
	FakeFetcher fetcher{clock};
	ExternalValues ext{loop, time, settings, connectivity, model, fetcher};

	Fixture() {
		clock.wall_seconds = 1792270800; // 2026-10-17
		clock.synchronized = true;
		settings.set_string(hc::setting::kLatitude, "48.1");
		settings.set_string(hc::setting::kLongitude, "11.6");
		connectivity.start();
		connectivity.link_up("192.0.2.100");
		DatapointSpec specs[] = {
			{"weather.temperature", ValueType::real, kR},          // 0
			{"weather.apparent", ValueType::real, kR},             // 1
			{"weather.humidity", ValueType::unsigned_int, kR},     // 2
			{"weather.precipitation", ValueType::real, kR},        // 3
			{"weather.rain", ValueType::real, kR},                 // 4
			{"weather.wind", ValueType::real, kR},                 // 5
			{"weather.wind_direction", ValueType::real, kR},       // 6
			{"weather.cloud_cover", ValueType::unsigned_int, kR},  // 7
			{"weather.pressure", ValueType::real, kR},             // 8
			{"weather.code", ValueType::enumeration, kR},          // 9
			{"weather.temperature_1h", ValueType::real, kR},       // 10
			{"weather.temperature_3h", ValueType::real, kR},       // 11
			{"weather.temperature_6h", ValueType::real, kR},       // 12
			{"weather.rain_1h", ValueType::real, kR},              // 13
			{"weather.rain_3h", ValueType::real, kR},              // 14
			{"weather.rain_6h", ValueType::real, kR},              // 15
			{"pv.power", ValueType::real, kR},                     // 16
			{"pv.online", ValueType::boolean, kR},                 // 17
			{"pv.count", ValueType::signed_int, kR},               // 18
			{"pv.serial", ValueType::string, kR},                  // 19
			{"clock.date", ValueType::date, kR},                   // 20
			{"setpoint", ValueType::real, kR | DatapointFlags::kWritable},                            // 21
			{"remote", ValueType::real, kR | DatapointFlags::kWritable | DatapointFlags::kTransmit}, // 22
		};
		REQUIRE(model.load(specs, sizeof(specs) / sizeof(specs[0])) == DatapointStatus::ok);
	}

	/// Runs the loop, jumping the clock from deadline to deadline, until `until_ms`.
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

	/// One source "pv" with a URL and the given bindings.
	bool configure_pv(const ExternalBindingSpec* bindings, size_t count, const char* url = "http://pv.example/status",
		uint32_t interval_s = 300) {
		ExternalSourceSpec source;
		source.id = "pv";
		source.url = url;
		source.interval_s = interval_s;
		source.bindings = bindings;
		source.binding_count = count;
		return ext.configure(&source, 1);
	}
};

ExternalBindingSpec bind_path(const char* datapoint, const char* path, double scale = 1, double offset = 0) {
	ExternalBindingSpec b;
	b.datapoint = datapoint;
	b.path = path;
	b.scale = scale;
	b.offset = offset;
	return b;
}

ExternalBindingSpec bind_variable(const char* datapoint, const char* variable) {
	ExternalBindingSpec b;
	b.datapoint = datapoint;
	b.variable = variable;
	return b;
}

const ExternalBindingSpec kPvPower[] = {bind_path("pv.power", "inverters[0].ac.power", 0.001)};

const char* kPvBody = R"({"online": true, "inverters": [{"serial": "A1", "ac": {"power": 2350, "voltage": 231.4}}]})";

JsonSpan parse(const char* text) {
	JsonSpan span;
	REQUIRE(hc::json_parse(text, std::strlen(text), span));
	return span;
}

} // namespace

TEST_CASE("ExternalPath: members, indexes and the refusals") {
	JsonSpan root = parse(R"({"a": {"b": [10, {"c": "x"}, [1, 2, 3]]}, "list": [], "n": null})");
	JsonSpan out;
	REQUIRE(hc::external_extract(root, "a.b[0]", out));
	double number = 0;
	CHECK(hc::json_number(out, number));
	CHECK(number == 10);
	REQUIRE(hc::external_extract(root, "a.b[1].c", out));
	CHECK(hc::json_string_equals(out, "x"));
	REQUIRE(hc::external_extract(root, "a.b[2][2]", out));
	CHECK(hc::json_number(out, number));
	CHECK(number == 3);
	REQUIRE(hc::external_extract(root, "n", out));
	CHECK(out.kind == JsonSpan::Kind::null);

	JsonSpan array = parse(R"([{"value": 7}])");
	REQUIRE(hc::external_extract(array, "[0].value", out));
	CHECK(hc::json_number(out, number));
	CHECK(number == 7);

	const char* error = nullptr;
	size_t at = 0;
	CHECK_FALSE(hc::external_extract(root, "a.x", out, &error, &at));
	CHECK(std::string(error) == "member missing");
	CHECK(at == 2);
	CHECK_FALSE(hc::external_extract(root, "a.b[3]", out, &error));
	CHECK(std::string(error) == "index out of range");
	CHECK_FALSE(hc::external_extract(root, "list[0]", out, &error));
	CHECK(std::string(error) == "index out of range");
	CHECK_FALSE(hc::external_extract(root, "a[0]", out, &error));
	CHECK(std::string(error) == "not an array");
	CHECK_FALSE(hc::external_extract(root, "a.b.c", out, &error));
	CHECK(std::string(error) == "not an object");

	CHECK(hc::external_path_valid("current.temperature_2m"));
	CHECK(hc::external_path_valid("[0]"));
	CHECK(hc::external_path_valid("a[1][2].b"));
	for (const char* bad : {"", ".a", "a.", "a..b", "a[]", "a[x]", "a[1]b", "a b", "a]", "a[123456]", "a.[0]"}) {
		CAPTURE(bad);
		CHECK_FALSE(hc::external_path_valid(bad, &error));
	}
	std::string long_path(64, 'a');
	CHECK_FALSE(hc::external_path_valid(long_path.c_str(), &error));
	CHECK(std::string(error) == "path longer than 63 bytes");
}

TEST_CASE("ExternalValues: the Open-Meteo preset fills every datapoint from the captured response") {
	Fixture f;
	size_t count = 0;
	const hc::ExternalPresetVariable* variables = hc::open_meteo_variables(count);
	REQUIRE(count == 16);
	const char* names[] = {"weather.temperature", "weather.apparent", "weather.humidity", "weather.precipitation",
		"weather.rain", "weather.wind", "weather.wind_direction", "weather.cloud_cover", "weather.pressure",
		"weather.code", "weather.temperature_1h", "weather.temperature_3h", "weather.temperature_6h",
		"weather.rain_1h", "weather.rain_3h", "weather.rain_6h"};
	std::vector<ExternalBindingSpec> bindings;
	for (size_t i = 0; i < count; ++i) {
		bindings.push_back(bind_variable(names[i], variables[i].name));
	}
	ExternalSourceSpec source;
	source.id = "weather";
	source.preset = "open-meteo";
	source.bindings = bindings.data();
	source.binding_count = bindings.size();
	uint64_t t0 = f.clock.now_ms;
	REQUIRE(f.ext.configure(&source, 1));

	f.run_until(t0 + ExternalValues::kStartDelayMs - 1);
	CHECK(f.fetcher.requests.empty());
	f.run_until(t0 + ExternalValues::kStartDelayMs);
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].url ==
		"https://api.open-meteo.com/v1/forecast?latitude=48.10&longitude=11.60&current=temperature_2m,"
		"apparent_temperature,relative_humidity_2m,precipitation,rain,wind_speed_10m,wind_direction_10m,cloud_cover,"
		"pressure_msl,weather_code&hourly=temperature_2m,precipitation&forecast_hours=7&wind_speed_unit=ms&timezone=UTC");
	CHECK(f.fetcher.requests[0].max_bytes == ExternalValues::kMaxBody);
	CHECK(f.ext.fetching());

	f.fetcher.ok(read_fixture("open-meteo-example.json"));
	CHECK_FALSE(f.ext.fetching());
	struct Expect {
		const char* name;
		double value;
	};
	const Expect expected[] = {
		{"weather.temperature", 10.4}, {"weather.apparent", 8.8}, {"weather.humidity", 92},
		{"weather.precipitation", 0}, {"weather.rain", 0}, {"weather.wind", 2.64}, {"weather.wind_direction", 331},
		{"weather.cloud_cover", 100}, {"weather.pressure", 1020.7}, {"weather.code", 3},
		{"weather.temperature_1h", 10.1}, {"weather.temperature_3h", 9.8}, {"weather.temperature_6h", 9.2},
		{"weather.rain_1h", 0.3}, {"weather.rain_3h", 0.4}, {"weather.rain_6h", 0},
	};
	for (const Expect& e : expected) {
		CAPTURE(e.name);
		const hc::Datapoint& dp = f.dp(e.name);
		CHECK(dp.quality() == Quality::good);
		switch (dp.type()) {
		case ValueType::real:
			CHECK(dp.value().as_real() == doctest::Approx(e.value));
			break;
		case ValueType::unsigned_int:
			CHECK(dp.value().as_unsigned() == static_cast<uint32_t>(e.value));
			break;
		case ValueType::enumeration:
			CHECK(dp.value().as_enum() == static_cast<uint32_t>(e.value));
			break;
		default:
			FAIL("unexpected type");
		}
	}
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(0, status));
	CHECK(status.fetches == 1);
	CHECK(status.failures == 0);
	CHECK(std::string(status.preset) == "open-meteo");
	CHECK(status.next_ms == t0 + ExternalValues::kStartDelayMs + 900000);
	hc::ExternalBindingStatus binding;
	REQUIRE(f.ext.binding(0, 0, binding));
	CHECK(std::string(binding.path) == "current.temperature_2m");
	CHECK(std::string(binding.unit) == "\xc2\xb0" "C");
	CHECK(f.capture.sink.contains("source 'weather': first fetch, 16 value(s)"));
}

TEST_CASE("ExternalValues: the preset URL asks only for what the bindings use") {
	Fixture f;
	ExternalBindingSpec bindings[] = {
		bind_variable("weather.temperature", "temperature"),
		bind_variable("weather.temperature_3h", "temperature_3h"),
		bind_variable("weather.temperature_1h", "temperature_1h"),
	};
	ExternalSourceSpec source;
	source.id = "weather";
	source.preset = "open-meteo";
	source.bindings = bindings;
	source.binding_count = 3;
	REQUIRE(f.ext.configure(&source, 1));
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(0, status));
	CHECK(std::string(status.url) ==
		"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m"
		"&hourly=temperature_2m&forecast_hours=4&wind_speed_unit=ms&timezone=UTC");
}

TEST_CASE("ExternalValues: sources are staggered, one fetch at a time, on their own grid") {
	Fixture f;
	ExternalBindingSpec a[] = {bind_path("pv.power", "p")};
	ExternalBindingSpec b[] = {bind_path("pv.count", "n")};
	ExternalBindingSpec c[] = {bind_path("weather.temperature", "t")};
	ExternalSourceSpec sources[3];
	sources[0].id = "a";
	sources[0].url = "http://a.example/";
	sources[0].interval_s = 300;
	sources[0].bindings = a;
	sources[0].binding_count = 1;
	sources[1].id = "b";
	sources[1].url = "http://b.example/";
	sources[1].interval_s = 600;
	sources[1].bindings = b;
	sources[1].binding_count = 1;
	sources[2].id = "c";
	sources[2].url = "http://c.example/";
	sources[2].bindings = c;
	sources[2].binding_count = 1;
	uint64_t t0 = f.clock.now_ms;
	REQUIRE(f.ext.configure(sources, 3));

	f.run_until(t0 + 5000);
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].url == "http://a.example/");
	// b is due at +25 s but a's fetch is still in flight: b waits for it.
	f.run_until(t0 + 30000);
	CHECK(f.fetcher.requests.size() == 1);
	f.fetcher.ok(R"({"p": 1})");
	f.run_for(0);
	REQUIRE(f.fetcher.requests.size() == 2);
	CHECK(f.fetcher.requests[1].url == "http://b.example/");
	CHECK(f.fetcher.requests[1].at == t0 + 30000);
	f.fetcher.ok(R"({"n": 2})");
	f.run_until(t0 + 45000);
	REQUIRE(f.fetcher.requests.size() == 3);
	CHECK(f.fetcher.requests[2].url == "http://c.example/");
	CHECK(f.fetcher.requests[2].at == t0 + 45000);
	f.fetcher.ok(R"({"t": 3})");

	// The grids: a every 300 s from +5 s, b every 600 s from +25 s (not from when it ran).
	f.run_until(t0 + 305000);
	REQUIRE(f.fetcher.requests.size() == 4);
	CHECK(f.fetcher.requests[3].url == "http://a.example/");
	CHECK(f.fetcher.requests[3].at == t0 + 305000);
	f.fetcher.ok(R"({"p": 1})");
	f.run_until(t0 + 625000);
	REQUIRE(f.fetcher.requests.size() == 5);
	CHECK(f.fetcher.requests[4].url == "http://a.example/");
	CHECK(f.fetcher.requests[4].at == t0 + 605000);
	f.fetcher.ok(R"({"p": 1})");
	f.run_for(0);
	REQUIRE(f.fetcher.requests.size() == 6);
	CHECK(f.fetcher.requests[5].url == "http://b.example/");
	CHECK(f.fetcher.requests[5].at == t0 + 625000);
}

TEST_CASE("ExternalValues: failures back off, the stale rule, recovery") {
	Fixture f;
	REQUIRE(f.configure_pv(kPvPower, 1));
	uint64_t t0 = f.clock.now_ms;
	f.run_until(t0 + 5000);
	REQUIRE(f.fetcher.requests.size() == 1);
	f.fetcher.ok(kPvBody);
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(2.35));
	CHECK(f.dp("pv.power").quality() == Quality::good);

	// First failure: still good, retried after a minute.
	f.run_until(t0 + 305000);
	REQUIRE(f.fetcher.requests.size() == 2);
	f.fetcher.fail(503, "HTTP status 503");
	CHECK(f.dp("pv.power").quality() == Quality::good);
	CHECK(f.capture.sink.count("source 'pv': HTTP status 503") == 1);
	f.run_until(t0 + 305000 + 59999);
	CHECK(f.fetcher.requests.size() == 2);
	f.run_until(t0 + 365000);
	REQUIRE(f.fetcher.requests.size() == 3);

	// Second failure with the same reason: logged once; the datapoint is stale.
	f.fetcher.fail(503, "HTTP status 503");
	CHECK(f.capture.sink.count("source 'pv': HTTP status 503") == 1);
	CHECK(f.dp("pv.power").quality() == Quality::stale);
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(2.35));
	CHECK(f.capture.sink.contains("source 'pv': 2 failures in a row: its 1 value(s) are stale"));

	// Backoff doubles: 2 min, then 4 min.
	f.run_until(t0 + 365000 + 120000);
	REQUIRE(f.fetcher.requests.size() == 4);
	f.fetcher.fail(0, "cannot connect: timeout");
	CHECK(f.capture.sink.contains("source 'pv': cannot connect: timeout"));
	f.run_until(t0 + 485000 + 239999);
	CHECK(f.fetcher.requests.size() == 4);
	f.run_until(t0 + 485000 + 240000);
	REQUIRE(f.fetcher.requests.size() == 5);

	// Recovery: good again, back on the 300 s grid from +5 s.
	f.fetcher.ok(R"({"inverters": [{"ac": {"power": 1000}}]})");
	CHECK(f.dp("pv.power").quality() == Quality::good);
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(1.0));
	CHECK(f.capture.sink.contains("source 'pv': recovered after 3 failure(s) (cannot connect: timeout)"));
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(0, status));
	CHECK(status.failures == 0);
	CHECK(std::string(status.last_error).empty());
	CHECK(status.next_ms == t0 + 905000);
	// The backoff starts over.
	f.run_until(t0 + 905000);
	REQUIRE(f.fetcher.requests.size() == 6);
	f.fetcher.fail(500);
	CHECK(f.capture.sink.contains("source 'pv': HTTP status 500"));
	REQUIRE(f.ext.status(0, status));
	CHECK(status.next_ms == t0 + 965000);
}

TEST_CASE("ExternalValues: nothing is fetched without network, synchronized clock or location") {
	Fixture f;
	f.connectivity.link_down("test");
	ExternalBindingSpec bindings[] = {bind_variable("weather.temperature", "temperature")};
	ExternalSourceSpec source;
	source.id = "weather";
	source.preset = "open-meteo";
	source.interval_s = 300;
	source.bindings = bindings;
	source.binding_count = 1;
	uint64_t t0 = f.clock.now_ms;
	REQUIRE(f.ext.configure(&source, 1));
	f.run_until(t0 + 400000);
	CHECK(f.fetcher.requests.empty());
	CHECK(f.capture.sink.count("source 'weather': not fetched: the network is down") == 1);
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(0, status));
	CHECK(status.failures == 2); // +5 s and +305 s missed

	// The network comes back (the Connectivity reconnects on its own): the fetch follows within a
	// poll period and the start delay.
	f.connectivity.link_up("192.0.2.100");
	uint64_t up = f.clock.now_ms;
	f.run_until(up + ExternalValues::kReadyPollMs + ExternalValues::kStartDelayMs);
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].at > up);
	f.fetcher.ok(read_fixture("open-meteo-example.json"));
	CHECK(f.dp("weather.temperature").value().as_real() == doctest::Approx(10.4));
	CHECK(f.capture.sink.contains("source 'weather': recovered after 2 failure(s)"));

	// Clock no longer synchronized: missed with that reason, and the value goes stale after two.
	f.clock.synchronized = false;
	f.run_for(700000);
	CHECK(f.fetcher.requests.size() == 1);
	CHECK(f.capture.sink.contains("not fetched: the clock is not synchronized (TLS needs the date)"));
	CHECK(f.dp("weather.temperature").quality() == Quality::stale);
	f.clock.synchronized = true;

	// No location: the preset (with placeholders) waits; a URL without placeholders does not.
	f.settings.erase(hc::setting::kLongitude);
	f.run_for(400000);
	CHECK(f.fetcher.requests.size() == 1);
	CHECK(f.capture.sink.contains("not fetched: no valid location (settings location.lat, location.lon)"));
	f.settings.set_string(hc::setting::kLongitude, "11.6");
	f.run_for(ExternalValues::kReadyPollMs + ExternalValues::kStartDelayMs);
	REQUIRE(f.fetcher.requests.size() == 2);
	f.fetcher.ok(read_fixture("open-meteo-example.json"));
	CHECK(f.dp("weather.temperature").quality() == Quality::good);

	Fixture g;
	g.settings.erase(hc::setting::kLatitude);
	REQUIRE(g.configure_pv(kPvPower, 1));
	g.run_for(5000);
	CHECK(g.fetcher.requests.size() == 1);
}

TEST_CASE("ExternalValues: scale, offset, rounding and the value kinds") {
	Fixture f;
	ExternalBindingSpec bindings[] = {
		bind_path("pv.power", "inverters[0].ac.voltage", 2, -0.4),
		bind_path("pv.count", "inverters[0].ac.voltage", -1),
		bind_path("pv.online", "online"),
		bind_path("pv.serial", "inverters[0].serial"),
		bind_path("weather.code", "inverters[0].ac.power", 0.001),
	};
	REQUIRE(f.configure_pv(bindings, 5));
	f.run_for(5000);
	f.fetcher.ok(kPvBody);
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(462.4));
	CHECK(f.dp("pv.count").value().as_signed() == -231);
	CHECK(f.dp("pv.online").value().as_bool());
	CHECK(std::string(f.dp("pv.serial").value().as_string()) == "A1");
	CHECK(f.dp("weather.code").value().as_enum() == 2); // 2.35 rounds to 2
}

TEST_CASE("ExternalValues: a response that does not fit reports nothing and says why") {
	struct Case {
		const char* body;
		ExternalBindingSpec binding;
		const char* reason;
	};
	const Case cases[] = {
		{R"({"online": true})", bind_path("pv.power", "inverters[0].ac.power"),
			"path 'inverters[0].ac.power': member missing at 'inverters[0].ac.power'"},
		{R"({"inverters": []})", bind_path("pv.power", "inverters[0].ac.power"),
			"path 'inverters[0].ac.power': index out of range at '[0].ac.power'"},
		{R"({"x": "21.5"})", bind_path("pv.power", "x"), "path 'x': a string for a datapoint that is not a string"},
		{R"({"x": null})", bind_path("pv.power", "x"), "path 'x': null (the service has no value)"},
		{R"({"x": {"y": 1}})", bind_path("pv.power", "x"), "path 'x': an object, not a value"},
		{R"({"x": 1})", bind_path("pv.online", "x"), "path 'x': a number for a datapoint that is not numeric"},
		{R"({"x": true})", bind_path("pv.power", "x"), "path 'x': a boolean for a datapoint that is not boolean"},
		{R"({"x": -1})", bind_path("weather.humidity", "x"), "path 'x': out of the unsigned datapoint's range"},
		{R"({"x": 1e300})", bind_path("pv.power", "x", 1e10), "path 'x': out of range after scaling"},
		{R"({"x": "a string longer than twenty-eight bytes"})", bind_path("pv.serial", "x"),
			"path 'x': a string longer than 28 bytes"},
		{R"(<html>)", bind_path("pv.power", "x"), "the response is not JSON"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.reason);
		Fixture f;
		ExternalBindingSpec single[] = {c.binding};
		REQUIRE(f.configure_pv(single, 1));
		f.run_for(5000);
		REQUIRE(f.fetcher.requests.size() == 1);
		f.fetcher.ok(c.body);
		CHECK(f.capture.sink.contains(std::string("source 'pv': ") + c.reason));
		CHECK(f.dp(c.binding.datapoint).quality() == Quality::unknown);
	}

	// All or none: the first binding would extract, the second does not.
	Fixture f;
	ExternalBindingSpec bindings[] = {bind_path("pv.online", "online"), bind_path("pv.power", "missing")};
	REQUIRE(f.configure_pv(bindings, 2));
	f.run_for(5000);
	f.fetcher.ok(kPvBody);
	CHECK(f.dp("pv.online").quality() == Quality::unknown);
	CHECK(f.capture.sink.contains("source 'pv': path 'missing': member missing at 'missing'"));
}

TEST_CASE("ExternalValues: the body bound, a fetcher refusal, a lost result") {
	Fixture f;
	REQUIRE(f.configure_pv(kPvPower, 1));
	uint64_t t0 = f.clock.now_ms;
	f.fetcher.refuse = "busy: a fetch is in flight";
	f.run_until(t0 + 5000);
	CHECK(f.fetcher.requests.empty());
	CHECK(f.capture.sink.contains("source 'pv': fetch not started: busy: a fetch is in flight"));
	f.fetcher.refuse = nullptr;

	// The platform names the size it refused.
	f.run_until(t0 + 65000);
	REQUIRE(f.fetcher.requests.size() == 1);
	f.fetcher.fail(200, "body larger than 8192 bytes");
	CHECK(f.capture.sink.contains("source 'pv': body larger than 8192 bytes"));

	// No result within the guard time: a failure; the late result is ignored.
	f.run_until(t0 + 185000);
	REQUIRE(f.fetcher.requests.size() == 2);
	f.run_for(ExternalValues::kFetchTimeoutMs);
	CHECK(f.capture.sink.contains("source 'pv': no result within 120 s"));
	CHECK_FALSE(f.ext.fetching());
	f.fetcher.ok(kPvBody);
	CHECK(f.capture.sink.contains("a fetch result arrived after its timeout: ignored"));
	CHECK(f.dp("pv.power").quality() == Quality::unknown);
}

TEST_CASE("ExternalValues: commands are refused") {
	Fixture f;
	ExternalBindingSpec bindings[] = {bind_path("remote", "x"), bind_path("pv.power", "y")};
	REQUIRE(f.configure_pv(bindings, 2));
	CHECK(f.model.command(f.dp("remote").id(), Value::of_real(1)) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("refusing command to 'remote': an external value (source 'pv') is read only"));
	CHECK(f.model.command(f.dp("pv.power").id(), Value::of_real(1)) == DatapointStatus::not_writable);
	CHECK(f.dp("remote").technology() == &f.ext);
}

TEST_CASE("ExternalValues: malformed source definitions are refused and keep the previous configuration") {
	Fixture f;
	REQUIRE(f.configure_pv(kPvPower, 1));

	struct Case {
		const char* url;
		const char* preset;
		uint32_t interval_s;
		ExternalBindingSpec binding;
		const char* reason;
	};
	const Case cases[] = {
		{"http://x.example/", nullptr, 299, bind_path("pv.count", "a"), "interval 299 s outside 300..86400 s"},
		{"http://x.example/", nullptr, 86401, bind_path("pv.count", "a"), "interval 86401 s outside"},
		{nullptr, "weather.com", 900, bind_variable("pv.count", "temperature"), "unknown preset 'weather.com'"},
		{"http://x.example/", "open-meteo", 900, bind_path("pv.count", "a"), "both a URL and a preset"},
		{nullptr, nullptr, 900, bind_path("pv.count", "a"), "neither a URL nor a preset"},
		{"ftp://x.example/", nullptr, 900, bind_path("pv.count", "a"), "not an http:// or https:// URL"},
		{"https:///path", nullptr, 900, bind_path("pv.count", "a"), "no host"},
		{"http://x.example/a b", nullptr, 900, bind_path("pv.count", "a"), "a space, quote"},
		{"http://x.example/?q={city}", nullptr, 900, bind_path("pv.count", "a"), "unknown placeholder"},
		{"http://x.example/?q=}", nullptr, 900, bind_path("pv.count", "a"), "a '}' without a placeholder"},
		{"http://x.example/", nullptr, 900, bind_path("nothing", "a"), "no datapoint 'nothing' in the model"},
		{"http://x.example/", nullptr, 900, bind_path("clock.date", "a"), "is of type date"},
		{"http://x.example/", nullptr, 900, bind_path("setpoint", "a"), "is writable without transmit"},
		{"http://x.example/", nullptr, 900, bind_path("pv.count", "a..b"), "path 'a..b' refused: an empty member name"},
		{"http://x.example/", nullptr, 900, bind_path("pv.count", "a", 0), "scale must be a finite number other than 0"},
		{"http://x.example/", nullptr, 900, bind_variable("pv.count", "temperature"), "takes a path, not a variable"},
		{nullptr, "open-meteo", 900, bind_path("pv.count", "current.x"), "takes a variable, not a path"},
		{nullptr, "open-meteo", 900, bind_variable("pv.count", "snow"), "unknown open-meteo variable 'snow'"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.reason);
		ExternalSourceSpec source;
		source.id = "other";
		source.url = c.url;
		source.preset = c.preset;
		source.interval_s = c.interval_s;
		source.bindings = &c.binding;
		source.binding_count = 1;
		const char* error = nullptr;
		CHECK_FALSE(f.ext.configure(&source, 1, &error));
		REQUIRE(error != nullptr);
		CHECK(std::string(error).find(c.reason) != std::string::npos);
		CHECK(f.capture.sink.contains(std::string("configuration refused: ") + error));
	}

	// Ids, duplicates, ownership, empty sources.
	const char* error = nullptr;
	ExternalBindingSpec one[] = {bind_path("pv.count", "a")};
	ExternalBindingSpec twice[] = {bind_path("pv.count", "a"), bind_path("pv.count", "b")};
	ExternalSourceSpec source;
	source.url = "http://x.example/";
	source.bindings = one;
	source.binding_count = 1;
	source.id = "bad id";
	CHECK_FALSE(f.ext.configure(&source, 1, &error));
	CHECK(std::string(error).find("the id must be 1..15 characters") != std::string::npos);
	source.id = "a-very-long-source-id";
	CHECK_FALSE(f.ext.configure(&source, 1, &error));
	CHECK(std::string(error).find("longer than 15") != std::string::npos);
	source.id = "x";
	ExternalSourceSpec pair[2] = {source, source};
	CHECK_FALSE(f.ext.configure(pair, 2, &error));
	CHECK(std::string(error).find("source 'x' is defined twice") != std::string::npos);
	source.bindings = twice;
	source.binding_count = 2;
	CHECK_FALSE(f.ext.configure(&source, 1, &error));
	CHECK(std::string(error).find("datapoint 'pv.count' is bound twice") != std::string::npos);
	source.binding_count = 0;
	CHECK_FALSE(f.ext.configure(&source, 1, &error));
	CHECK(std::string(error).find("source 'x' has no bindings") != std::string::npos);
	ExternalSourceSpec many[ExternalValues::kMaxSources + 1];
	CHECK_FALSE(f.ext.configure(many, ExternalValues::kMaxSources + 1, &error));
	CHECK(std::string(error).find("9 sources, at most 8") != std::string::npos);

	// Owned by another technology.
	struct Other : hc::Technology {
		const char* name() const override { return "knx"; }
		bool command(const hc::Datapoint&, const Value&) override { return true; }
	} other;
	REQUIRE(f.model.bind(f.dp("pv.count").id(), other) == DatapointStatus::ok);
	source.bindings = one;
	source.binding_count = 1;
	CHECK_FALSE(f.ext.configure(&source, 1, &error));
	CHECK(std::string(error).find("datapoint 'pv.count' is owned by knx") != std::string::npos);

	// The first configuration still runs.
	REQUIRE(f.ext.size() == 1);
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(0, status));
	CHECK(std::string(status.id) == "pv");
	f.run_for(5000);
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].url == "http://pv.example/status");
}

TEST_CASE("ExternalValues: the image's external member (spec/fixtures/external/image-external.json)") {
	Fixture f;
	std::string image = read_fixture("image-external.json");
	const char* error = nullptr;
	REQUIRE(f.ext.configure_json(image.data(), image.size(), &error));
	REQUIRE(f.ext.size() == 2);
	ExternalSourceStatus status;
	REQUIRE(f.ext.status(1, status));
	CHECK(std::string(status.id) == "pv");
	CHECK(status.interval_s == 300);
	hc::ExternalBindingStatus binding;
	REQUIRE(f.ext.binding(1, 0, binding));
	CHECK(binding.scale == 0.001);
	CHECK(std::string(binding.unit) == "kW");

	uint64_t t0 = f.clock.now_ms;
	f.run_until(t0 + 5000);
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].url ==
		"https://api.open-meteo.com/v1/forecast?latitude=48.10&longitude=11.60&current=temperature_2m,"
		"relative_humidity_2m,wind_speed_10m,weather_code&hourly=temperature_2m,precipitation&forecast_hours=4"
		"&wind_speed_unit=ms&timezone=UTC");
	f.fetcher.ok(read_fixture("open-meteo-example.json"));
	CHECK(f.dp("weather.temperature").value().as_real() == doctest::Approx(10.4));
	CHECK(f.dp("weather.humidity").value().as_unsigned() == 92);
	CHECK(f.dp("weather.wind").value().as_real() == doctest::Approx(2.64));
	CHECK(f.dp("weather.code").value().as_enum() == 3);
	CHECK(f.dp("weather.temperature_3h").value().as_real() == doctest::Approx(9.8));
	CHECK(f.dp("weather.rain_1h").value().as_real() == doctest::Approx(0.3));

	f.run_until(t0 + 25000);
	REQUIRE(f.fetcher.requests.size() == 2);
	CHECK(f.fetcher.requests[1].url == "http://pv.example/api/status.json");
	f.fetcher.ok(read_fixture("pv-example.json"));
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(2.35));
	CHECK(f.dp("pv.online").value().as_bool());
}

TEST_CASE("ExternalValues: malformed external members are refused") {
	struct Case {
		const char* json;
		const char* reason;
	};
	const Case cases[] = {
		{"[]", "the external member must be an object"},
		{R"({"sources": {}})", "\"sources\" must be an array"},
		{R"({"source": []})", "unknown member \"source\""},
		{R"({"sources": [1]})", "source 1 is not an object"},
		{R"({"sources": [{"url": "http://x.example/"}]})", "source 1: \"id\" is missing"},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "bindings": [], "period": 5}]})",
			"source 'x': unknown member \"period\""},
		{R"({"sources": [{"id": "x", "url": "http://x.example/"}]})", "\"bindings\" must be an array"},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "interval": 300.5, "bindings": []}]})",
			"\"interval\" must be a whole number of seconds"},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "bindings": [{"datapoint": "pv.power"}]}]})",
			"\"path\" is missing"},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "bindings": [{"datapoint": "pv.power", "path": "a", "scale": "2"}]}]})",
			"\"scale\" must be a number"},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "bindings": [{"datapoint": "pv.power", "path": "a", "factor": 2}]}]})",
			"unknown member \"factor\""},
		{R"({"sources": [{"id": "x", "preset": "open-meteo", "bindings": [{"datapoint": "pv.power", "path": "a"}]}]})",
			"a preset source takes \"variable\", not \"path\""},
		{R"({"sources": [{"id": "x", "url": "http://x.example/", "interval": 60, "bindings": [{"datapoint": "pv.power", "path": "a"}]}]})",
			"interval 60 s outside 300..86400 s"},
		{R"({"sources": [)", "not JSON"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.json);
		Fixture f;
		const char* error = nullptr;
		CHECK_FALSE(f.ext.configure_json(c.json, std::strlen(c.json), &error));
		REQUIRE(error != nullptr);
		CHECK(std::string(error).find(c.reason) != std::string::npos);
		CHECK(f.ext.size() == 0);
	}
}

TEST_CASE("ExternalValues: fetch now, its refusals, and a reconfiguration while a fetch is in flight") {
	Fixture f;
	REQUIRE(f.configure_pv(kPvPower, 1));
	const char* error = nullptr;
	CHECK_FALSE(f.ext.fetch_now("nope", &error));
	CHECK(std::string(error) == "no source with this id");
	uint64_t t0 = f.clock.now_ms;
	REQUIRE(f.ext.fetch_now("pv", &error));
	REQUIRE(f.fetcher.requests.size() == 1);
	CHECK(f.fetcher.requests[0].at == t0);
	CHECK_FALSE(f.ext.fetch_now("pv", &error));
	CHECK(std::string(error) == "its fetch is in flight");
	f.fetcher.ok(kPvBody);
	f.clock.advance(30000);
	CHECK_FALSE(f.ext.fetch_now("pv", &error));
	CHECK(std::string(error) == "its last attempt was less than a minute ago (rate limit)");
	f.clock.advance(30000);
	f.connectivity.link_down("test");
	CHECK_FALSE(f.ext.fetch_now("pv", &error));
	CHECK(std::string(error) == "not fetched: the network is down");
	f.connectivity.link_up("192.0.2.100");
	REQUIRE(f.ext.fetch_now("pv", &error));
	REQUIRE(f.fetcher.requests.size() == 2);

	// Reconfigured while the fetch is in flight: its result is dropped, the new source waits for it.
	ExternalBindingSpec other[] = {bind_path("pv.count", "n")};
	REQUIRE(f.configure_pv(other, 1, "http://other.example/"));
	f.run_for(10000);
	CHECK(f.fetcher.requests.size() == 2);
	f.fetcher.ok(kPvBody);
	CHECK(f.capture.sink.contains("the result of a fetch for a replaced configuration is dropped"));
	CHECK(f.dp("pv.power").value().as_real() == doctest::Approx(2.35)); // the earlier value, not reported again
	f.run_for(0);
	REQUIRE(f.fetcher.requests.size() == 3);
	CHECK(f.fetcher.requests[2].url == "http://other.example/");

	f.ext.clear();
	CHECK(f.ext.size() == 0);
	CHECK_FALSE(f.ext.fetch_now("pv", &error));
	CHECK(std::string(error) == "no external sources configured");
}
