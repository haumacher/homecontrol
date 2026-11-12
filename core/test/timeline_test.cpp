/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The timelines of rules with states (spec/rules.md 2.8, 7.13, 8.7, 8.8; issue #84): what the
// corpus (cases 74 to 100) does not cover. Every refusal of the table of spec 10.1 with its path
// and text, the moments of points (a missing sun event, a moment kept within its date), the
// canonical form, and the image and the API end to end with the heating program of
// spec/fixtures/images/heating-timeline.json.
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/rules/engine.h"
#include "hc/rules/timeline.h"
#include "hc/web_api.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc_test::Json;
using hc::rules::LoadError;
using hc::rules::Timeline;
using hc::rules::TimelineFound;
using hc::rules::TimelineRefusal;

namespace {

class States : public hc::rules::TimelineStates {
public:
	size_t count() const override { return 3; }
	const char* id(size_t index) const override {
		static const char* const kIds[] = {"comfort", "standby", "night"};
		return kIds[index];
	}
};

/// UTC as local time (the TimeSource's defaults), with sun events set per test.
class Utc : public hc::rules::TimeSource {
public:
	bool local_now(hc::DateTime&) const override { return false; }
	bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
	SunLookup sun_event_utc(int32_t year, unsigned month, unsigned day, hc::SunEvent event,
		int64_t& out) const override {
		if (!location) {
			return SunLookup::no_location;
		}
		if (event == hc::SunEvent::dusk && no_dusk) {
			return SunLookup::absent; // a summer night too bright for civil dusk
		}
		// Sunrise 06:00, sunset 18:00, dusk 18:30, dawn 05:30 every day.
		int64_t base = hc::rules::days_of(year, month, day) * 86400;
		int64_t seconds = event == hc::SunEvent::dawn ? 19800 : event == hc::SunEvent::sunrise ? 21600
			: event == hc::SunEvent::sunset ? 64800 : 66600;
		out = base + seconds;
		return SunLookup::ok;
	}
	bool location = true;
	bool no_dusk = false;
};

bool parse(const std::string& text, Timeline* out, TimelineRefusal& refusal, bool image = true) {
	States states;
	return Timeline::parse_text(text.data(), text.size(), states, image, out, refusal);
}

const char* kPlan = R"({"id": "day", "label": "Day", "points": [{"time": {"hour": 6, "minute": 0, "second": 0}, "state": "comfort"}]})";

std::string with_plans(const std::string& plans, const std::string& assignments = R"([{"plan": "day"}])") {
	return R"({"plans": )" + plans + R"(, "assignments": )" + assignments + "}";
}

std::string with_point(const std::string& point) {
	return with_plans(R"([{"id": "day", "label": "Day", "points": [)" + point + "]}]");
}

std::string clock_point(int minute, const char* state = "comfort") {
	return R"({"time": {"hour": 6, "minute": )" + std::to_string(minute) + R"(, "second": 0}, "state": ")" + state +
		"\"}";
}

} // namespace

TEST_CASE("Timeline.Refusals: every check of spec/rules.md 10.1 with its reason, path and text") {
	struct Row {
		std::string json;
		LoadError error;
		const char* path;
		const char* text;
	};
	std::string many_points;
	for (int i = 0; i < 17; ++i) {
		many_points += (i > 0 ? ", " : "") + clock_point(i);
	}
	std::string full_plans;
	for (int p = 0; p < 5; ++p) {
		std::string points;
		for (int i = 0; i < 13; ++i) {
			points += (i > 0 ? ", " : "") + clock_point(i);
		}
		full_plans += std::string(p > 0 ? ", " : "") + R"({"id": "p)" + std::to_string(p) + R"(", "label": "P", "points": [)" +
			points + "]}";
	}
	std::string nine_plans;
	for (int p = 0; p < 9; ++p) {
		nine_plans += std::string(p > 0 ? ", " : "") + R"({"id": "p)" + std::to_string(p) + R"(", "label": "P", "points": []})";
	}
	const std::vector<Row> rows = {
		{"[]", LoadError::malformed, "", "an object expected"},
		{R"({"plans": [], "assignments": [], "x": 1})", LoadError::malformed, "", "unknown member 'x'"},
		{R"({"adjustable": 1, "plans": [], "assignments": []})", LoadError::malformed, "adjustable",
			"'adjustable' must be a boolean"},
		{R"({"assignments": []})", LoadError::malformed, "", "member 'plans' missing"},
		{with_plans("[]"), LoadError::malformed, "plans", "a timeline needs at least one plan"},
		{with_plans("[" + nine_plans + "]"), LoadError::limit_exceeded, "plans", "more than 8 plans"},
		{with_plans(R"([{"id": "a b", "label": "L", "points": []}])"), LoadError::malformed, "plans[0].id",
			"plan id 'a b' is not 1..24 characters of A-Z a-z 0-9 _ -"},
		{with_plans(std::string("[") + kPlan + ", " + kPlan + "]"), LoadError::duplicate_id, "plans[1].id",
			"plan id 'day' is used twice"},
		{with_plans(R"([{"id": "day", "points": []}])"), LoadError::malformed, "plans[0]", "member 'label' missing"},
		{with_plans(R"([{"id": "day", "label": {"en": "a", "de": "b", "fr": "c", "it": "d", "es": "e"}, "points": []}])"),
			LoadError::limit_exceeded, "plans[0].label.es", "more than 4 languages in a plan's label"},
		{with_plans(R"([{"id": "day", "label": "Day"}])"), LoadError::malformed, "plans[0]", "member 'points' missing"},
		{with_plans(R"([{"id": "day", "label": "Day", "points": [)" + many_points + "]}]"), LoadError::limit_exceeded,
			"plans[0].points", "more than 16 points in a plan"},
		{with_point(R"({"state": "comfort"})"), LoadError::malformed, "plans[0].points[0]", "a point needs 'time' or 'sun'"},
		{with_point(R"({"time": {"hour": 6, "minute": 0, "second": 0}, "sun": "dusk", "state": "comfort"})"),
			LoadError::malformed, "plans[0].points[0]", "a point has 'time' or 'sun', not both"},
		{with_point(R"({"time": {"hour": 24, "minute": 0, "second": 0}, "state": "comfort"})"), LoadError::malformed,
			"plans[0].points[0].time", "'time' must be a time {hour 0..23, minute 0..59, second 0..59}"},
		{with_point(R"({"sun": "noon", "state": "comfort"})"), LoadError::malformed, "plans[0].points[0].sun",
			"unknown sun event 'noon' (dawn, sunrise, sunset, dusk)"},
		{with_point(R"({"time": {"hour": 6, "minute": 0, "second": 0}, "notAfter": {"hour": 6, "minute": 0, "second": 0}, "state": "comfort"})"),
			LoadError::malformed, "plans[0].points[0].notAfter", "'notAfter' is only for a sun point"},
		{with_point(R"({"sun": "dusk", "offset": 1500, "state": "comfort"})"), LoadError::malformed,
			"plans[0].points[0].offset", "an offset must be whole seconds within -12 h..12 h"},
		{with_point(R"({"sun": "dusk", "offset": 43201000, "state": "comfort"})"), LoadError::malformed,
			"plans[0].points[0].offset", "an offset must be whole seconds within -12 h..12 h"},
		{with_point(R"({"sun": "dusk", "notBefore": {"hour": 6, "minute": 0}, "state": "comfort"})"), LoadError::malformed,
			"plans[0].points[0].notBefore", "'notBefore' must be a time {hour 0..23, minute 0..59, second 0..59}"},
		{with_point(R"({"sun": "dusk", "notBefore": {"hour": 22, "minute": 0, "second": 0}, "notAfter": {"hour": 21, "minute": 0, "second": 1}, "state": "comfort"})"),
			LoadError::malformed, "plans[0].points[0]", "notBefore 22:00:00 is after notAfter 21:00:01"},
		{with_point(R"({"sun": "dusk"})"), LoadError::malformed, "plans[0].points[0]", "member 'state' missing"},
		{with_point(R"({"sun": "dusk", "state": "warm"})"), LoadError::unknown_reference, "plans[0].points[0].state",
			"no state 'warm' in the rule"},
		{with_plans("[" + full_plans + "]", R"([{"plan": "p0"}])"), LoadError::limit_exceeded, "",
			"more than 64 points in a timeline"},
		{R"({"plans": [)" + std::string(kPlan) + "]}", LoadError::malformed, "", "member 'assignments' missing"},
		{with_plans(std::string("[") + kPlan + "]",
			R"([{"plan": "day"}, {"days": [1], "plan": "day"}, {"days": [2], "plan": "day"}, {"days": [3], "plan": "day"}, {"days": [4], "plan": "day"}, {"days": [5], "plan": "day"}, {"days": [6], "plan": "day"}, {"days": [7], "plan": "day"}, {"days": [7], "plan": "day"}])"),
			LoadError::limit_exceeded, "assignments", "more than 8 assignments"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"days": [1]}])"), LoadError::malformed, "assignments[0]",
			"member 'plan' missing"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"plan": "night"}])"), LoadError::unknown_reference,
			"assignments[0].plan", "no plan 'night' in the timeline"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"plan": "day"}, {"days": [], "plan": "day"}])"),
			LoadError::malformed, "assignments[1].days", "'days' must be a non-empty array of weekdays 1..7"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"plan": "day"}, {"days": [0], "plan": "day"}])"),
			LoadError::malformed, "assignments[1].days[0]", "weekday 0 is not 1..7"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"plan": "day"}, {"days": [3, 3], "plan": "day"}])"),
			LoadError::malformed, "assignments[1].days[1]", "weekday 3 is listed twice"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"days": [6], "plan": "day"}, {"plan": "day"}, {"plan": "day"}])"),
			LoadError::malformed, "assignments[2]", "a second default assignment (the first is assignments[1])"},
		{with_plans(std::string("[") + kPlan + "]", R"([{"days": [6], "plan": "day"}])"), LoadError::malformed,
			"assignments", "no default assignment (one without 'days')"},
	};
	for (const Row& row : rows) {
		TimelineRefusal refusal;
		Timeline timeline;
		CHECK_MESSAGE(!parse(row.json, &timeline, refusal), row.json);
		CHECK_MESSAGE(refusal.error == row.error, row.json << ": " << hc::rules::load_error_name(refusal.error));
		CHECK_MESSAGE(std::string(refusal.path) == row.path, row.json << ": path " << refusal.path);
		CHECK_MESSAGE(std::string(refusal.text) == row.text, row.json << ": " << refusal.text);
		CHECK(timeline.empty());
	}
	// The inhabitant's timeline does not carry `adjustable` (spec 8.7).
	TimelineRefusal refusal;
	CHECK_FALSE(parse(R"({"adjustable": true, "plans": [], "assignments": []})", nullptr, refusal, false));
	CHECK(std::string(refusal.text) == "unknown member 'adjustable'");
	// Allowed: an empty plan, an unassigned plan, two points at one moment.
	CHECK(parse(with_plans(std::string("[") + kPlan + R"(, {"id": "spare", "label": "Spare", "points": []}])"), nullptr,
		refusal));
	CHECK(parse(with_point(clock_point(0) + ", " + clock_point(0, "night")), nullptr, refusal));
}

TEST_CASE("Timeline.Canonical form: the members in order, an offset 0 left out, days sorted; equality") {
	const std::string text = R"({"adjustable": false, "plans": [{"label": {"en": "Day", "de": "Tag \"1\""}, "id": "day", "points": [
		{"state": "night", "sun": "sunset", "offset": 0, "notAfter": {"second": 0, "minute": 0, "hour": 22}},
		{"state": "comfort", "time": {"hour": 6, "minute": 30, "second": 0}}]}],
		"assignments": [{"plan": "day", "days": [7, 6]}, {"plan": "day"}]})";
	Timeline a;
	TimelineRefusal refusal;
	REQUIRE(parse(text, &a, refusal));
	CHECK_FALSE(a.adjustable());
	States states;
	size_t length = 0;
	char* canonical = a.canonical_text(states, length);
	REQUIRE(canonical != nullptr);
	CHECK(std::string(canonical) ==
		R"({"plans":[{"id":"day","label":{"en":"Day","de":"Tag \"1\""},"points":[{"sun":"sunset","notAfter":{"hour":22,"minute":0,"second":0},"state":"night"},{"time":{"hour":6,"minute":30,"second":0},"state":"comfort"}]}],"assignments":[{"days":[6,7],"plan":"day"},{"plan":"day"}]})");
	Timeline b;
	REQUIRE(Timeline::parse_text(canonical, length, states, false, &b, refusal));
	CHECK(a.equals(b));
	delete[] canonical;
	Timeline c;
	REQUIRE(parse(with_point(clock_point(30)), &c, refusal));
	CHECK_FALSE(a.equals(c));
	CHECK(a.memory() > 0);
}

TEST_CASE("Timeline.Moments: a missing sun event, no location, a moment kept within its date, the window") {
	Utc time;
	TimelineRefusal refusal;
	Timeline t;
	// Sunrise + 12 h with nothing to stop it would be 18:00 the same day; dusk + 6 h would cross
	// midnight: kept at 23:59:59 of its date (spec 7.13 point 1).
	REQUIRE(parse(with_point(R"({"sun": "dusk", "offset": 21600000, "state": "night"}, {"sun": "dawn", "offset": -21600000, "state": "comfort"})"),
		&t, refusal));
	int64_t day = hc::rules::days_of(2026, 11, 18);
	TimelineFound points[16];
	bool no_location = false;
	size_t n = hc::rules::timeline_points_of(t, time, day, points, no_location);
	REQUIRE(n == 2);
	CHECK(points[0].moment == day * 86400); // dawn 05:30 - 6 h: kept at 00:00:00
	CHECK(points[0].state == 0);
	CHECK(points[1].moment == day * 86400 + 86399); // dusk 18:30 + 6 h: kept at 23:59:59
	CHECK_FALSE(no_location);

	// Without dusk on a date the point has no moment; the other points still apply.
	time.no_dusk = true;
	n = hc::rules::timeline_points_of(t, time, day, points, no_location);
	CHECK(n == 1);
	TimelineFound found;
	REQUIRE(hc::rules::timeline_due(t, time, day * 86400 + 43200, found));
	CHECK(found.state == 0);
	// Without a location no sun point has a moment: no due state, and it says why.
	time.location = false;
	CHECK_FALSE(hc::rules::timeline_due(t, time, day * 86400 + 43200, found));
	CHECK(found.no_location);
	CHECK_FALSE(hc::rules::timeline_next(t, time, day * 86400, found));

	// The window: a point only on Sundays is found from the Sunday a week later before its moment
	// (seven dates back), and the next one after it.
	Timeline sundays;
	REQUIRE(parse(with_plans(R"([{"id": "none", "label": "N", "points": []}, {"id": "sun", "label": "S", "points": [{"time": {"hour": 10, "minute": 0, "second": 0}, "state": "night"}]}])",
		R"([{"plan": "none"}, {"days": [7], "plan": "sun"}])"), &sundays, refusal));
	int64_t sunday = hc::rules::days_of(2026, 11, 22);
	REQUIRE(hc::rules::timeline_due(sundays, time, sunday * 86400 + 9 * 3600, found));
	CHECK(found.moment == (sunday - 7) * 86400 + 10 * 3600);
	REQUIRE(hc::rules::timeline_next(sundays, time, sunday * 86400 + 11 * 3600, found));
	CHECK(found.moment == (sunday + 7) * 86400 + 10 * 3600);
	// Points between two instants, whole dates at a time.
	size_t between = hc::rules::timeline_between(sundays, time, (sunday - 8) * 86400, (sunday + 8) * 86400, points, 16,
		no_location);
	CHECK(between == 3);
}

// ---- The image and the API ------------------------------------------------------------------------

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

struct TimelineFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	std::string cookie;

	TimelineFixture() {
		fake.clock.wall_seconds = 1792231392; // Saturday 2026-10-17, 11:23 UTC
		fake.clock.synchronized = true;
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void load(const std::string& text) {
		REQUIRE(service.load(text.data(), text.size(), hc::image::Source::api, true));
		controller.run_once(); // the start cascades
		controller.run_once();
	}

	Result put(const std::string& target, const std::string& body) {
		return request(HttpMethod::put, target, body, cookie);
	}

	/// The datapoint entry of /api/model named `name` (every page).
	Json model_datapoint(const std::string& name) {
		uint32_t from = 0;
		for (int page = 0; page < 20; ++page) {
			Result r = get("/api/model?from=" + std::to_string(from), cookie);
			REQUIRE(r.status == 200);
			for (const Json& d : r.json["datapoints"].items) {
				if (d["name"].text == name) {
					return d;
				}
			}
			if (r.json["next"].kind == Json::Kind::null) {
				break;
			}
			from = static_cast<uint32_t>(r.json["next"].number);
		}
		return Json();
	}
};

const char* kTimelineBody = R"({"timeline": {"plans": [
	{"id": "workday", "label": {"en": "Workday", "de": "Werktag"}, "points": [
		{"time": {"hour": 6, "minute": 30, "second": 0}, "state": "comfort"},
		{"time": {"hour": 8, "minute": 0, "second": 0}, "state": "standby"},
		{"sun": "sunset", "offset": 3600000, "notAfter": {"hour": 22, "minute": 0, "second": 0}, "state": "night"}]},
	{"id": "weekend", "label": {"en": "Weekend", "de": "Wochenende"}, "points": [
		{"time": {"hour": 7, "minute": 30, "second": 0}, "state": "comfort"},
		{"time": {"hour": 23, "minute": 0, "second": 0}, "state": "night"}]}],
	"assignments": [{"plan": "workday"}, {"days": [6, 7], "plan": "weekend"}]}})";

} // namespace

TEST_CASE("Timeline.Image: the heating program of haumacher/ehome #140 through the image, the model and the API") {
	TimelineFixture f;
	f.load(fixture("heating-timeline.json"));

	// The state datapoint (spec 8.8): an enumeration of the position, read-only, the states' labels.
	Json dp = f.model_datapoint("rule/heating/state");
	REQUIRE(dp.kind == Json::Kind::object);
	CHECK(dp["type"].text == "enumeration");
	CHECK_FALSE(dp["writable"].boolean);
	CHECK(dp["technology"].text == "rules");
	CHECK(dp["state"]["rule"].text == "heating");
	CHECK(dp["label"]["de"].text == "Heizprogramm");
	REQUIRE(dp["choices"].items.size() == 3);
	CHECK(dp["choices"].items[2]["value"].number == 2);
	CHECK(dp["choices"].items[2]["label"]["en"].text == "Night");
	// Saturday 11:23 UTC is in the weekend's comfort (07:30): entered at the boot with its entry actions.
	auto r = f.get("/api/datapoints/" + std::to_string(static_cast<int>(dp["id"].number)), f.cookie);
	CHECK(r.json["value"].number == 0);
	CHECK(f.model.find("1/4/1")->value().as_enum() == 1);
	r = f.post("/api/datapoints/" + std::to_string(static_cast<int>(dp["id"].number)) + "/command", R"({"value": 2})",
		f.cookie);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "dp.not_writable");

	// GET /api/rules: the states and the timeline's summary.
	r = f.get("/api/rules", f.cookie);
	REQUIRE(r.status == 200);
	const Json& rule = r.json["rules"].items[0];
	CHECK(rule["state"].text == "comfort");
	REQUIRE(rule["states"].items.size() == 3);
	CHECK(rule["states"].items[1]["label"]["de"].text == "Standby");
	CHECK(rule["timeline"]["adjustable"].boolean);
	CHECK_FALSE(rule["timeline"]["overridden"].boolean);
	CHECK(rule["timeline"]["plans"].number == 2);
	CHECK(rule["timeline"]["next"]["state"].text == "night");
	CHECK(rule["timeline"]["next"]["plan"].text == "weekend");

	// GET the timeline, paged by plan.
	r = f.get("/api/rules/heating/timeline", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["source"].text == "effective");
	CHECK(r.json["count"].number == 2);
	CHECK(r.json["plans"].items.size() == 2);
	CHECK(r.json["assignments"].items[1]["days"].items.size() == 2);
	CHECK(r.json["next"].kind == Json::Kind::null);
	r = f.get("/api/rules/heating/timeline?from=1", f.cookie);
	CHECK(r.json["plans"].items[0]["id"].text == "weekend");
	CHECK(f.get("/api/rules/heating/timeline?source=other", f.cookie).status == 400);
	CHECK(f.get("/api/rules/nothing/timeline", f.cookie).json["code"].text == "rule.unknown");

	// PUT: refused whole with the reason and the path.
	r = f.put("/api/rules/heating/timeline", R"({"timeline": {"plans": [{"id": "d", "label": "D", "points": [{"time": {"hour": 6, "minute": 0, "second": 0}, "state": "warm"}]}], "assignments": [{"plan": "d"}]}})");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "rule.unknown_state");
	CHECK(r.json["path"].text == "plans[0].points[0].state");
	CHECK(r.json["error"].text == "unknownReference: no state 'warm' in the rule");
	r = f.put("/api/rules/heating/timeline", R"({"timeline": {"adjustable": false, "plans": [], "assignments": []}})");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "rule.malformed_timeline");
	r = f.put("/api/rules/heating/timeline", R"({"timeline": {"plans": [], "assignments": []}, "plans": []})");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "rule.malformed_timeline");
	r = f.put("/api/rules/heating/timeline", R"({})");
	CHECK(r.json["code"].text == "rule.value_missing");
	CHECK(f.put("/api/rules/heating/timeline", "{").json["code"].text == "api.bad_json");
	CHECK(f.request(HttpMethod::post, "/api/rules/heating/timeline", "{}", f.cookie).status == 405);
	CHECK(f.api.body_limit(HttpMethod::put, "/api/rules/heating/timeline") == hc::WebApi::kMaxTimelineBody);
	CHECK(f.api.body_limit(HttpMethod::put, "/api/rules/heating/parameters/x") == hc::WebApi::kMaxBody);

	// PUT a timeline: it applies at once (Saturday 11:23: the weekend plan, comfort again: a
	// self-transition that sends the mode again), the override is persisted.
	r = f.put("/api/rules/heating/timeline", kTimelineBody);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["rule"]["timeline"]["overridden"].boolean);
	CHECK(r.json["rule"]["state"].text == "comfort");
	r = f.get("/api/rules/heating/timeline", f.cookie);
	CHECK(r.json["overridden"].boolean);
	CHECK(r.json["plans"].items[0]["points"].items[0]["time"]["minute"].number == 30);
	CHECK(r.json["plans"].items[0]["points"].items[2]["offset"].number == 3600000);
	r = f.get("/api/rules/heating/timeline?source=image", f.cookie);
	CHECK(r.json["plans"].items[0]["points"].items[0]["time"]["minute"].number == 0);
	f.service.engine().persist_now();
	char name[hc::rules::Engine::kTimelineBlobName];
	hc::rules::Engine::timeline_blob_name("heating", name);
	size_t size = 0;
	REQUIRE(f.blobs.size(name, size) == hc::BlobStatus::ok);
	CHECK(size > 100);

	// A new image with the same states keeps it; one without the state `night` drops it.
	std::string again = fixture("heating-timeline.json");
	again.replace(again.find("\"revision\": 1"), 13, "\"revision\": 2");
	f.load(again);
	CHECK(f.get("/api/rules", f.cookie).json["rules"].items[0]["timeline"]["overridden"].boolean);
	std::string renamed = again;
	for (size_t at = renamed.find("\"night\""); at != std::string::npos; at = renamed.find("\"night\"", at)) {
		renamed.replace(at, 7, "\"sleep\"");
	}
	renamed.replace(renamed.find("\"revision\": 2"), 13, "\"revision\": 3");
	f.load(renamed);
	r = f.get("/api/rules", f.cookie);
	CHECK_FALSE(r.json["rules"].items[0]["timeline"]["overridden"].boolean);
	f.service.engine().persist_now();
	CHECK(f.blobs.size(name, size) == hc::BlobStatus::not_found);

	// Override and reset.
	f.load(again);
	REQUIRE(f.put("/api/rules/heating/timeline", kTimelineBody).status == 200);
	r = f.request(HttpMethod::del, "/api/rules/heating/timeline", "", f.cookie);
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["rule"]["timeline"]["overridden"].boolean);
}

TEST_CASE("Timeline.Image: a fixed timeline is refused 403; a rule without a timeline 404") {
	TimelineFixture f;
	std::string text = fixture("heating-timeline.json");
	text.replace(text.find("\"adjustable\": true"), 18, "\"adjustable\": false");
	f.load(text);
	auto r = f.put("/api/rules/heating/timeline", kTimelineBody);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "rule.not_adjustable");
	CHECK(f.request(HttpMethod::del, "/api/rules/heating/timeline", "", f.cookie).status == 403);
	CHECK(f.get("/api/rules/heating/timeline", f.cookie).status == 200);
}

TEST_CASE("Timeline.Image: a parameter state of a rule with states: refused from 1.7, its datapoint before") {
	const char* kImage = R"({"image": {"schemaVersion": "%s", "projectId": "p", "revision": 1},
		"datapoints": [{"id": "x", "type": "boolean", "writable": true, "virtual": {}}],
		"rules": [{"id": "r", "name": "R", "parameters": [{"id": "state", "label": "S", "type": "integer", "default": 1}],
			"states": [{"id": "a"}, {"id": "b"}], "initialState": "a",
			"reactions": [{"triggers": [{"kind": "start"}], "actions": [], "to": "b"}]}]})";
	char text[1024];
	{
		TimelineFixture f;
		std::snprintf(text, sizeof(text), kImage, "1.6");
		f.load(text);
		Json dp = f.model_datapoint("rule/r/state");
		REQUIRE(dp.kind == Json::Kind::object);
		CHECK(dp["type"].text == "signed"); // the parameter's datapoint, not the state's
		CHECK(dp["parameter"]["parameter"].text == "state");
	}
	{
		TimelineFixture f;
		std::snprintf(text, sizeof(text), kImage, "1.7");
		CHECK_FALSE(f.service.load(text, std::strlen(text), hc::image::Source::api, true));
	}
}
