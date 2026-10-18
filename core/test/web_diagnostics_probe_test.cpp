/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the System page's route (issue #42) composed with the principals and a platform that
// has some facts and not others: a deploy token reads it, exactly one task is the loop's, and a
// missing sensor says so beside present chip facts.
#include <string>

#include "doctest.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

struct ProbeFixture : WebFixture {
	std::string cookie;
	ProbeFixture() {
		controller.start();
		cookie = onboard();
	}
};

hc::TaskInfo task(const char* name, hc::TaskState state, int32_t priority, int64_t stack, int32_t cpu, bool current) {
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

TEST_CASE("WebDiagnostics probe: a deploy token reads the page, one task is the loop's, a missing sensor speaks") {
	ProbeFixture f;
	f.fake.system.list_tasks = true;
	f.fake.system.tasks = {
		task("IDLE", hc::TaskState::ready, 0, 812, 900, false),
		task("hc_loop", hc::TaskState::running, 5, 3412, 87, true),
		task("httpd", hc::TaskState::blocked, 5, 2100, 4, false),
	};
	f.fake.system.has_temperature = false;
	f.fake.system.chip = true;

	auto created = f.request(HttpMethod::post, "/api/tokens", "{\"name\": \"ehome\"}", f.cookie, "", true);
	REQUIRE_MESSAGE(created.status == 200, created.body);
	std::string bearer = "Bearer " + created.json["token"].text;
	auto r = f.request(HttpMethod::get, "/api/system/diagnostics", "", "", bearer, false);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& j = r.json;

	size_t current = 0;
	for (const auto& t : j["tasks"]["list"].items) {
		if (t.has("current") && t["current"].boolean) {
			++current;
			CHECK(t["name"].text == "hc_loop");
		}
	}
	CHECK(current == 1);
	CHECK(j["tasks"]["count"].number == 3);
	CHECK_FALSE(j["tasks"].has("omitted"));

	CHECK(j["chip"]["available"].boolean);
	CHECK_FALSE(j["temperature"]["available"].boolean);
	CHECK_FALSE(j["temperature"]["reason"].text.empty());

	// The loop's counters are present from the start, at zero on an idle fixture.
	CHECK(j["loop"].has("missed"));
	CHECK(j["loop"]["longest_turn_ms"].number >= 0);

	// A revoked token loses the page; the open session keeps it.
	REQUIRE(f.request(HttpMethod::del, "/api/tokens/ehome", "", f.cookie, "", true).status == 200);
	CHECK(f.request(HttpMethod::get, "/api/system/diagnostics", "", "", bearer, false).status == 401);
	CHECK(f.get("/api/system/diagnostics", f.cookie).status == 200);
}

TEST_CASE("WebDiagnostics probe: the console output queue (issue #46) with its drops, or the reason it is missing") {
	ProbeFixture f;
	// The host build writes stdout directly: no queue, and the section says why.
	auto r = f.get("/api/system/diagnostics", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK_FALSE(r.json["console"]["available"].boolean);
	CHECK_FALSE(r.json["console"]["reason"].text.empty());
	CHECK_FALSE(r.json["console"].has("dropped"));

	// The device's queue: the figures as the platform reports them, a 64-bit count included, and
	// the task list still behind it.
	f.fake.system.has_console = true;
	f.fake.system.console.dropped = 5000000000ull;
	f.fake.system.console.dropped_unread = 4999999990ull;
	f.fake.system.console.queued = 812;
	f.fake.system.console.capacity = 4096;
	f.fake.system.console.host = true;
	f.fake.system.list_tasks = true;
	f.fake.system.tasks = {task("hc_out", hc::TaskState::blocked, 3, 1400, 1, false),
		task("hc_loop", hc::TaskState::running, 5, 3412, 87, true)};
	r = f.get("/api/system/diagnostics", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	const auto& c = r.json["console"];
	CHECK(c["available"].boolean);
	CHECK(c["host"].boolean);
	CHECK(c["dropped"].number == 5000000000.0);
	CHECK(c["dropped_unread"].number == 4999999990.0);
	CHECK(c["queued"].number == 812);
	CHECK(c["capacity"].number == 4096);
	CHECK(r.json["tasks"]["count"].number == 2);
	CHECK(r.json["loop"].has("missed"));
}
