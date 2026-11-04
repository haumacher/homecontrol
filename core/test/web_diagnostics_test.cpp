/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The System page's route (issue #42, spec/api.md "System diagnostics"): GET /api/system/diagnostics
// composes the platform's facts and the core's counters; what the platform does not have is
// "available": false with the reason.

#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

struct DiagnosticsFixture : WebFixture {
	std::string cookie;

	DiagnosticsFixture() {
		controller.start();
		cookie = onboard();
	}

	Result diagnostics() { return get("/api/system/diagnostics", cookie); }
};

hc::TaskInfo task(const char* name, hc::TaskState state, int32_t priority, int64_t stack, int32_t cpu,
	bool current = false) {
	hc::TaskInfo info;
	info.name = name;
	info.state = state;
	info.priority = priority;
	info.stack_free = stack;
	info.cpu_permille = cpu;
	info.current = current;
	return info;
}

} // namespace

TEST_CASE("WebDiagnostics: needs a principal and GET") {
	DiagnosticsFixture f;
	auto r = f.get("/api/system/diagnostics");
	CHECK(r.status == 401);
	CHECK(r.json["code"].text == "auth.required");
	r = f.request(HttpMethod::post, "/api/system/diagnostics", "", f.cookie);
	CHECK(r.status == 405);
	CHECK(r.json["code"].text == "api.method_not_allowed");

	// A token principal reads it as well.
	r = f.request(HttpMethod::post, "/api/tokens", "{\"name\": \"tool\"}", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	r = f.get("/api/system/diagnostics", "", "Bearer " + r.json["token"].text);
	CHECK(r.status == 200);
}

TEST_CASE("WebDiagnostics: the structure, with available false where the platform has nothing") {
	DiagnosticsFixture f;
	auto r = f.diagnostics();
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& j = r.json;
	for (const char* member : {"uptime_s", "now_ms", "device", "boot", "update", "chip", "led", "time", "improv",
		"memory", "temperature", "network", "wifi", "storage", "knx", "zigbee", "tasks", "loop"}) {
		CHECK_MESSAGE(j.has(member), member);
	}
	CHECK(j["device"]["name"].text == "homecontrol-a1b2c3");
	CHECK(j["boot"]["reason"].text == "unknown"); // the fake platform has no Diagnostics

	// The fake reports heap figures, nothing else.
	CHECK(j["memory"]["available"].boolean);
	CHECK(j["memory"]["free"].number == 300000);
	CHECK(j["memory"]["min_free"].number == 250000);
	CHECK_FALSE(j["memory"].has("largest")); // unknown

	for (const char* member : {"temperature", "chip", "wifi", "tasks", "knx", "zigbee"}) {
		CAPTURE(member);
		CHECK(j[member]["available"].kind == hc_test::Json::Kind::boolean);
		CHECK_FALSE(j[member]["available"].boolean);
		CHECK_FALSE(j[member]["reason"].text.empty());
	}
	CHECK(j["temperature"]["reason"].text == "this platform has no temperature sensor");
	CHECK(j["tasks"]["reason"].text == "this platform does not list its tasks");
	CHECK(j["wifi"]["reason"].text == "this network driver reports no link details");
	CHECK_FALSE(j["storage"]["available"].boolean); // no storage usage, no blob store
	CHECK_FALSE(j["storage"]["nvs"]["available"].boolean);
	CHECK_FALSE(j["storage"]["nvs"]["reason"].text.empty());

	// The core's own figures are always there.
	CHECK(j["loop"]["missed"].number == 0);
	CHECK(j["loop"].has("longest_turn_ms"));
	CHECK(j["loop"]["timer_capacity"].number == hc::EventLoop::kTimerCapacity);
	CHECK(j["loop"]["timers"].number > 0);
	CHECK_FALSE(j["loop"].has("stack_free")); // no task list, no loop task
	CHECK(j["network"]["state"].text == "connecting");
	CHECK(j["network"]["link_ups"].number == 0);
	CHECK(j["led"].text == "connecting");
	CHECK(j["time"].has("synchronized"));
	CHECK(j["improv"].has("active"));
}

TEST_CASE("WebDiagnostics: the platform's tasks, temperature and chip facts appear") {
	DiagnosticsFixture f;
	f.fake.system.largest_block = 110592;
	f.fake.system.list_tasks = true;
	f.fake.system.tasks = {
		task("hc_loop", hc::TaskState::running, 5, 3412, 87, true),
		task("IDLE", hc::TaskState::ready, 0, 812, 900),
		task("httpd", hc::TaskState::blocked, 5, 2100, 4),
		task("wifi", hc::TaskState::suspended, 23, -1, -1),
	};
	f.fake.system.has_temperature = true;
	f.fake.system.reading.celsius = 41.5;
	f.fake.system.reading.accuracy = 2;
	f.fake.system.reading.source = "die sensor";
	f.fake.system.chip = true;

	auto r = f.diagnostics();
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& j = r.json;
	CHECK(j["memory"]["largest"].number == 110592);

	const auto& tasks = j["tasks"];
	CHECK(tasks["available"].boolean);
	CHECK(tasks["count"].number == 4);
	CHECK(tasks["cpu"].boolean);
	CHECK_FALSE(tasks.has("omitted"));
	REQUIRE(tasks["list"].items.size() == 4);
	const auto& loop_task = tasks["list"].items[0];
	CHECK(loop_task["name"].text == "hc_loop");
	CHECK(loop_task["state"].text == "running");
	CHECK(loop_task["priority"].number == 5);
	CHECK(loop_task["stack_free"].number == 3412);
	CHECK(loop_task["cpu_permille"].number == 87);
	CHECK(loop_task["current"].boolean);
	CHECK(tasks["list"].items[1]["state"].text == "ready");
	CHECK_FALSE(tasks["list"].items[1].has("current"));
	CHECK(tasks["list"].items[2]["state"].text == "blocked");
	const auto& wifi_task = tasks["list"].items[3];
	CHECK(wifi_task["state"].text == "suspended");
	CHECK_FALSE(wifi_task.has("stack_free")); // unknown figures are left out
	CHECK_FALSE(wifi_task.has("cpu_permille"));

	// The loop's stack reserve is the calling task's.
	CHECK(j["loop"]["stack_free"].number == 3412);

	CHECK(j["temperature"]["available"].boolean);
	CHECK(j["temperature"]["celsius"].number == 41.5);
	CHECK(j["temperature"]["accuracy"].number == 2);
	CHECK(j["temperature"]["source"].text == "die sensor");
	CHECK(f.fake.system.temperature_reads == 1); // one reading per request

	CHECK(j["chip"]["available"].boolean);
	CHECK(j["chip"]["model"].text == "ESP32-C6");
	CHECK(j["chip"]["cpu_mhz"].number == 160);

	f.diagnostics();
	CHECK(f.fake.system.temperature_reads == 2);
}

TEST_CASE("WebDiagnostics: a long task list stops before the buffer is full and says how many it left out") {
	DiagnosticsFixture f;
	f.fake.system.list_tasks = true;
	static const char* const kNames[] = {"task_with_a_long_name_0", "task_with_a_long_name_1"};
	for (int i = 0; i < 300; ++i) {
		f.fake.system.tasks.push_back(task(kNames[i % 2], hc::TaskState::blocked, 3, 1000 + i, 1, i == 299));
	}
	auto r = f.diagnostics();
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& tasks = r.json["tasks"];
	CHECK(tasks["count"].number == 300);
	REQUIRE(tasks.has("omitted"));
	CHECK(tasks["omitted"].number + tasks["list"].items.size() == 300);
	CHECK(tasks["list"].items.size() > 10);
	// The loop's reserve comes from the calling task even when its entry was left out.
	CHECK(r.json["loop"]["stack_free"].number == 1299);
}

TEST_CASE("WebDiagnostics: the loop's counters, the link's reconnects, the storage and the KNX tunnel") {
	DiagnosticsFixture f;
	hc::Connectivity& net = f.controller.connectivity();
	net.link_up("192.0.2.10");
	f.fake.clock.advance(5000);
	net.link_down("test: access point gone");
	net.link_up("192.0.2.10");
	f.fake.clock.advance(7000);

	// A slow periodic handler: 250 ms in its first turn, its period 100 ms.
	struct Slow {
		hc_test::FakeClock* clock;
		int runs = 0;
		void run() {
			if (++runs == 1) {
				clock->advance(250);
			}
		}
	} slow{&f.fake.clock};
	f.controller.run_once(); // the controller's own timers catch up with the 12 s (their skips counted here)
	uint64_t missed_before = f.controller.loop().missed_periods();
	uint64_t start = f.fake.clock.now_ms;
	f.controller.loop().schedule_periodic(start, 100, hc::Callback::member<Slow, &Slow::run>(&slow));
	f.controller.run_once();
	f.controller.run_once();
	REQUIRE(slow.runs == 2);

	hc_test::FakeBlobStore blobs;
	blobs.has_usage = true;
	blobs.total_bytes = 1048576;
	blobs.used_bytes = 65536;
	hc::StorageStats storage(blobs);
	f.api.set_storage_stats(&storage);
	hc_test::FakeUdpSocket socket{f.fake.clock};
	hc::knx::KnxTunnel tunnel{f.controller.loop(), f.fake.clock, f.controller.connectivity(), socket};
	f.api.set_knx(&tunnel, nullptr);

	auto r = f.diagnostics();
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& j = r.json;
	CHECK(j["loop"]["missed"].number == missed_before + 1); // the deadline start+200 was skipped
	CHECK(j["loop"]["longest_turn_ms"].number >= 250);
	CHECK(j["loop"]["longest_turn_at_ms"].number == start);
	CHECK(j["loop"]["longest_turn_ago_s"].number == 0);

	CHECK(j["network"]["state"].text == "up");
	CHECK(j["network"]["ip"].text == "192.0.2.10");
	CHECK(j["network"]["link_ups"].number == 2);
	CHECK(j["network"]["reconnects"].number == 1);
	CHECK(j["network"]["up_for_s"].number == 7);

	CHECK(j["storage"]["available"].boolean);
	CHECK(j["storage"]["total"].number == 1048576);
	CHECK(j["storage"]["files"].number == 0);

	CHECK(j["knx"]["available"].boolean);
	CHECK(j["knx"]["state"].text == "down");
	CHECK(j["knx"]["connections"].number == 0);
	CHECK(j["knx"]["reconnects"].number == 0);
	CHECK_FALSE(j["knx"].has("channel")); // only while up
}
