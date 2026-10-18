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
