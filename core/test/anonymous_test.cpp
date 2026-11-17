/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The roles anonymous / operator / administrator (homecontrol #91, spec/api.md "Roles"): the image's
// anonymous pages (spec/runtime-image.md I12, 3.9, 9.16, format 1.1) say what may be used without
// sign-in. The compiler derives the sections from the pages' minimum role; the device validates them
// structurally, serves the anonymous body unchanged and checks every anonymous request against the
// tables. The endpoint matrix is table-driven and checked against the endpoints spec/api.md documents,
// so an endpoint without a decision fails the suite; the live feed is restricted the same way.

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/image/validator.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/websocket.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc::Role;
using hc_test::Json;
using hc_test::WebFixture;
namespace bin = hc::image::bin;

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// `text` without the part from `from` (included) up to `to` (excluded).
std::string cut(const std::string& text, const std::string& from, const std::string& to) {
	size_t a = text.find(from);
	REQUIRE_MESSAGE(a != std::string::npos, "not in the text: " << from);
	size_t b = text.find(to, a);
	REQUIRE_MESSAGE(b != std::string::npos, "not in the text: " << to);
	return text.substr(0, a) + text.substr(b);
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
	for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
		text.replace(at, from.size(), to);
	}
	return text;
}

/// The anonymous-pages fixture without any page of minimum role anonymous (they become operator pages).
std::string without_anonymous() {
	return replace_all(fixture("anonymous-pages.json"), "\"minimumRole\": \"anonymous\"", "\"minimumRole\": \"operator\"");
}

// ---- the bytes ------------------------------------------------------------------------------------

uint16_t u16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
	return static_cast<uint32_t>(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16)) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
	b[at] = static_cast<uint8_t>(v);
	b[at + 1] = static_cast<uint8_t>(v >> 8);
}

/// The position of section `id`'s entry in the section table; 0 when absent.
size_t entry_of(const std::vector<uint8_t>& b, uint16_t id) {
	for (size_t k = 0; k < u16(b, 22); ++k) {
		size_t at = u16(b, 20) + 16 * k;
		if (u16(b, at) == id) {
			return at;
		}
	}
	return 0;
}

std::string section(const std::vector<uint8_t>& b, uint16_t id) {
	size_t at = entry_of(b, id);
	return at == 0 ? std::string() : std::string(reinterpret_cast<const char*>(b.data()) + u32(b, at + 4), u32(b, at + 8));
}

void fix_crc(std::vector<uint8_t>& b) {
	uint32_t crc = bin::image_crc(b.data(), b.size());
	for (size_t i = 0; i < 4; ++i) {
		b[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}
}

/// The structural checks of section 10 on `bytes`.
bool structure(const std::vector<uint8_t>& bytes, hc::image::Report& report) {
	return hc::image::check_crc(bytes.data(), report) && hc::image::check_structure(bytes.data(), report);
}

bool refused_at(const hc::image::Report& report, const char* path, const char* reason) {
	for (size_t i = 0; i < report.count; ++i) {
		if (std::string(report.refusals[i].path) == path && std::strstr(report.refusals[i].reason, reason) != nullptr) {
			return true;
		}
	}
	return false;
}

// ---- the API and the live feed with the image service ---------------------------------------------

struct AnonymousFixture : WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot;
	hc::image::ImageService service{controller, model, blobs, slot, adapter, &tunnel};
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{api, controller.loop(), transport};
	std::string cookie;

	AnonymousFixture() {
		fake.clock.wall_seconds = 1792231392; // 2026-10-17
		fake.clock.synchronized = true;
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
		REQUIRE(feed.start());
	}

	void load(const std::string& text) {
		REQUIRE_MESSAGE(hc_test::deploy(service, text), hc_test::describe(service.report()));
		run();
	}

	int id(const char* name) const {
		const hc::Datapoint* dp = model.find(name);
		REQUIRE_MESSAGE(dp != nullptr, "no datapoint " << name);
		return dp->id();
	}

	/// A request without session and token (the role anonymous).
	Result anonymous(HttpMethod method, const std::string& target, const std::string& body = "", bool csrf = true) {
		return request(method, target, body, "", "", csrf);
	}

	std::string section_text(bool anonymous_pages) const {
		size_t length = 0;
		const char* text = anonymous_pages ? service.image().ui_anonymous(length) : service.image().ui(length);
		return text != nullptr ? std::string(text, length) : std::string();
	}

	void run() {
		controller.loop().run_once(fake.clock.now_ms);
		controller.loop().run_once(fake.clock.now_ms);
	}

	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(100);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}

	void send(uint32_t client, const std::string& text) { feed.message(client, text.data(), text.size()); }

	/// The ids of the datapoints the client's snapshots and updates carried, in order.
	std::set<int> carried(uint32_t client) const {
		std::set<int> ids;
		for (const Json& message : transport.messages(client, "datapoints")) {
			for (const Json& update : message["updates"].items) {
				ids.insert(static_cast<int>(update["id"].number));
			}
		}
		return ids;
	}
};

// ---- the endpoint matrix --------------------------------------------------------------------------

/// One endpoint as `anonymous` makes it with the image's anonymous pages: the status and code it gets.
/// `pattern` is the endpoint as spec/api.md names it; `opened` whether minimum_role() opens it.
struct Row {
	HttpMethod method;
	const char* target; ///< "{name}" is replaced by the id of the datapoint `name`.
	const char* body;
	int status;
	const char* code; ///< nullptr: any (a success)
	const char* pattern;
	bool opened;
};

constexpr const char* kRequired = "auth.required";

const Row kMatrix[] = {
	// The open endpoints, as for everybody.
	{HttpMethod::get, "/api/auth/session", "", 200, nullptr, "/api/auth/session", false},
	{HttpMethod::post, "/api/auth/onboard", "{\"password\":\"another horse\"}", 409, "auth.password_set", "/api/auth/onboard", false},
	{HttpMethod::post, "/api/auth/login", "{\"password\":\"wrong horse!\"}", 401, "auth.wrong_password", "/api/auth/login", false},
	{HttpMethod::post, "/api/auth/logout", "{}", 200, nullptr, "/api/auth/logout", false},
	// What the anonymous pages show.
	{HttpMethod::get, "/api/pages", "", 200, nullptr, "/api/pages", true},
	{HttpMethod::get, "/api/model", "", 200, nullptr, "/api/model", true},
	{HttpMethod::get, "/api/datapoints", "", 200, nullptr, "/api/datapoints", true},
	{HttpMethod::get, "/api/datapoints/{living.light}", "", 200, nullptr, "/api/datapoints/<id>", true},
	{HttpMethod::get, "/api/datapoints/living.light", "", 200, nullptr, "/api/datapoints/<id>", true},
	{HttpMethod::get, "/api/datapoints/{garage.door}", "", 404, "dp.unknown_id", "/api/datapoints/<id>", true},
	{HttpMethod::get, "/api/datapoints/garage.door", "", 404, "dp.unknown_id", "/api/datapoints/<id>", true},
	{HttpMethod::get, "/api/datapoints/{away}", "", 404, "dp.unknown_id", "/api/datapoints/<id>", true},
	{HttpMethod::post, "/api/datapoints/{living.light}/command", "{\"value\":true}", 200, nullptr, "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/living.light/command", "{\"value\":false}", 200, nullptr, "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/{1/3/1}/command", "{\"value\":20}", 409, "dp.not_writable", "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/{valve}/command", "{\"value\":20}", 409, "dp.not_writable", "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/{garage.door}/command", "{\"value\":true}", 404, "dp.unknown_id", "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/{rule/heat/setpoint}/command", "{\"value\":22}", 200, nullptr, "/api/datapoints/<id>/command", true},
	{HttpMethod::post, "/api/datapoints/{rule/heat/night}/command", "{\"value\":17}", 404, "dp.unknown_id", "/api/datapoints/<id>/command", true},
	{HttpMethod::get, "/api/history?ids={1/3/1},{1/3/5}&from=1792145000&to=1792231400", "", 200, nullptr, "/api/history", true},
	{HttpMethod::get, "/api/history?ids={1/3/5},{garage.door}&from=1792145000&to=1792231400", "", 404, "dp.unknown_id", "/api/history", true},
	{HttpMethod::put, "/api/rules/heat/parameters/setpoint", "{\"value\":22.5}", 200, nullptr, "/api/rules/<rule>/parameters/<parameter>", true},
	{HttpMethod::put, "/api/rules/heat/parameters/night", "{\"value\":17}", 404, "rule.unknown_parameter", "/api/rules/<rule>/parameters/<parameter>", true},
	{HttpMethod::put, "/api/rules/heat/parameters/enabled", "{\"value\":false}", 404, "rule.unknown_parameter", "/api/rules/<rule>/parameters/<parameter>", true},
	{HttpMethod::put, "/api/rules/presence/parameters/enabled", "{\"value\":false}", 404, "rule.unknown", "/api/rules/<rule>/parameters/<parameter>", true},
	{HttpMethod::put, "/api/rules/nosuch/parameters/x", "{\"value\":1}", 404, "rule.unknown", "/api/rules/<rule>/parameters/<parameter>", true},
	// Everything else: 401 before the routing.
	{HttpMethod::post, "/api/auth/sessions/clear", "{}", 401, kRequired, "/api/auth/sessions/clear", false},
	{HttpMethod::get, "/api/status", "", 401, kRequired, "/api/status", false},
	{HttpMethod::get, "/api/log", "", 401, kRequired, "/api/log", false},
	{HttpMethod::get, "/api/settings", "", 401, kRequired, "/api/settings", false},
	{HttpMethod::put, "/api/settings/tz", "{\"value\":\"UTC0\"}", 401, kRequired, "/api/settings/<key>", false},
	{HttpMethod::post, "/api/system/restart", "{}", 401, kRequired, "/api/system/restart", false},
	{HttpMethod::post, "/api/system/factory-reset", "{\"confirm\":\"homecontrol-a1b2c3\"}", 401, kRequired, "/api/system/factory-reset", false},
	{HttpMethod::post, "/api/system/update/check", "{}", 401, kRequired, "/api/system/update/check", false},
	{HttpMethod::post, "/api/system/update/now", "{}", 401, kRequired, "/api/system/update/now", false},
	{HttpMethod::post, "/api/system/update/upload", "{}", 401, kRequired, "/api/system/update/upload", false},
	{HttpMethod::get, "/api/system/log-level", "", 401, kRequired, "/api/system/log-level", false},
	{HttpMethod::put, "/api/system/log-level", "{\"tag\":\"hc.web\",\"level\":\"debug\"}", 401, kRequired, "/api/system/log-level", false},
	{HttpMethod::put, "/api/system/wifi", "{\"ssid\":\"x\",\"psk\":\"12345678\"}", 401, kRequired, "/api/system/wifi", false},
	{HttpMethod::get, "/api/system/diagnostics", "", 401, kRequired, "/api/system/diagnostics", false},
	{HttpMethod::get, "/api/coredump", "", 401, kRequired, "/api/coredump", false},
	{HttpMethod::del, "/api/coredump", "", 401, kRequired, "/api/coredump", false},
	{HttpMethod::get, "/api/tokens", "", 401, kRequired, "/api/tokens", false},
	{HttpMethod::post, "/api/tokens", "{\"name\":\"x\"}", 401, kRequired, "/api/tokens", false},
	{HttpMethod::del, "/api/tokens/x", "", 401, kRequired, "/api/tokens/<name>", false},
	{HttpMethod::post, "/api/external/x/fetch", "{}", 401, kRequired, "/api/external/<id>/fetch", false},
	{HttpMethod::post, "/api/lang/sync", "{}", 401, kRequired, "/api/lang/sync", false},
	{HttpMethod::post, "/api/mqtt/discovery", "{}", 401, kRequired, "/api/mqtt/discovery", false},
	{HttpMethod::get, "/api/knx/search", "", 401, kRequired, "/api/knx/search", false},
	{HttpMethod::post, "/api/knx/search", "{}", 401, kRequired, "/api/knx/search", false},
	{HttpMethod::get, "/api/zigbee", "", 401, kRequired, "/api/zigbee", false},
	{HttpMethod::get, "/api/zigbee/export", "", 401, kRequired, "/api/zigbee/export", false},
	{HttpMethod::post, "/api/zigbee/permit", "{\"seconds\":60}", 401, kRequired, "/api/zigbee/permit", false},
	{HttpMethod::del, "/api/zigbee/devices/0011223344556677", "", 401, kRequired, "/api/zigbee/devices/<ieee>", false},
	{HttpMethod::post, "/api/zigbee/devices/0011223344556677/identify", "{}", 401, kRequired, "/api/zigbee/devices/<ieee>", false},
	{HttpMethod::put, "/api/zigbee/devices/0011223344556677/label", "{\"label\":\"x\"}", 401, kRequired, "/api/zigbee/devices/<ieee>", false},
	{HttpMethod::post, "/api/zigbee/devices/0011223344556677/interview", "{}", 401, kRequired, "/api/zigbee/devices/<ieee>", false},
	{HttpMethod::put, "/api/image", "HCIM", 401, kRequired, "/api/image", false},
	{HttpMethod::get, "/api/image", "", 401, kRequired, "/api/image", false},
	{HttpMethod::del, "/api/image", "", 401, kRequired, "/api/image", false},
	{HttpMethod::get, "/api/rules", "", 401, kRequired, "/api/rules", false},
	{HttpMethod::get, "/api/rules/heat/timeline", "", 401, kRequired, "/api/rules/<rule>/timeline", false},
	{HttpMethod::put, "/api/rules/heat/timeline", "{\"timeline\":{}}", 401, kRequired, "/api/rules/<rule>/timeline", false},
	{HttpMethod::del, "/api/rules/heat/timeline", "", 401, kRequired, "/api/rules/<rule>/timeline", false},
	{HttpMethod::get, "/api/schedules", "", 401, kRequired, "/api/schedules", false},
	{HttpMethod::put, "/api/schedules/x", "{\"enabled\":false}", 401, kRequired, "/api/schedules/<id>", false},
	{HttpMethod::del, "/api/schedules/x/override", "", 401, kRequired, "/api/schedules/<id>/override", false},
	{HttpMethod::get, "/api/blocks", "", 401, kRequired, "/api/blocks", false},
	{HttpMethod::get, "/api/blocks/types", "", 401, kRequired, "/api/blocks/types", false},
	{HttpMethod::put, "/api/blocks/x", "{\"enabled\":false}", 401, kRequired, "/api/blocks/<id>", false},
	{HttpMethod::get, "/api/monitor", "", 401, kRequired, "/api/monitor", false},
	{HttpMethod::del, "/api/ehome", "", 401, kRequired, "/api/ehome", false},
	{HttpMethod::post, "/api/ehome/enroll", "{\"url\":\"https://ehome.example\",\"code\":\"ABCD-EFGH\"}", 401, kRequired, "/api/ehome/enroll", false},
	{HttpMethod::post, "/api/ehome/key/accept", "{\"fingerprint\":\"90ae 3702 f3a9 63ab\"}", 401, kRequired, "/api/ehome/key/accept", false},
	// Other methods on the opened paths, and paths the API does not know.
	{HttpMethod::post, "/api/pages", "{}", 401, kRequired, "/api/pages", false},
	{HttpMethod::put, "/api/model", "{}", 401, kRequired, "/api/model", false},
	{HttpMethod::del, "/api/datapoints/{living.light}", "", 401, kRequired, "/api/datapoints/<id>", false},
	{HttpMethod::put, "/api/datapoints/{living.light}/command", "{\"value\":true}", 401, kRequired, "/api/datapoints/<id>/command", false},
	{HttpMethod::post, "/api/rules/heat/parameters/setpoint", "{\"value\":22}", 401, kRequired, "/api/rules/<rule>/parameters/<parameter>", false},
	{HttpMethod::get, "/api/nosuch", "", 401, kRequired, "/api/nosuch", false},
	{HttpMethod::get, "/api/", "", 401, kRequired, "/api/", false},
};

std::string target_of(AnonymousFixture& f, const char* target) {
	std::string out = target;
	for (size_t open = out.find('{'); open != std::string::npos; open = out.find('{')) {
		size_t close = out.find('}', open);
		std::string name = out.substr(open + 1, close - open - 1);
		out.replace(open, close - open + 1, std::to_string(f.id(name.c_str())));
	}
	return out;
}

const char* method_text(HttpMethod method) {
	switch (method) {
	case HttpMethod::get: return "GET";
	case HttpMethod::post: return "POST";
	case HttpMethod::put: return "PUT";
	case HttpMethod::del: return "DELETE";
	default: return "?";
	}
}

/// Every `/api/...` path the headings of spec/api.md name, with the query and options removed
/// (`GET /api/datapoints[?from=<id>]` gives `/api/datapoints`).
std::set<std::string> documented_endpoints() {
	std::string text;
	REQUIRE(hc_test::read_text(HC_SPEC_DIR "/api.md", text));
	std::set<std::string> paths;
	size_t line = 0;
	while (line < text.size()) {
		size_t end = text.find('\n', line);
		end = end == std::string::npos ? text.size() : end;
		std::string heading = text.substr(line, end - line);
		line = end + 1;
		if (heading.rfind("### ", 0) != 0) {
			continue;
		}
		for (size_t at = heading.find("/api/"); at != std::string::npos; at = heading.find("/api/", at + 1)) {
			size_t stop = heading.find_first_of("`[? ", at);
			paths.insert(heading.substr(at, stop == std::string::npos ? std::string::npos : stop - at));
		}
	}
	return paths;
}

} // namespace

// ---- the compiler (3.9, 9.16) ---------------------------------------------------------------------

TEST_CASE("Anonymous.CompilerWritesTheSections") {
	// The fixture: two anonymous pages (home, climate), an explicit operator page and one by default.
	hc_test::CompiledImage image;
	REQUIRE(image.compile(fixture("anonymous-pages.json")));
	const std::vector<uint8_t>& b = image.bytes;
	CHECK(u16(b, 6) == 1); // format 1.1: the image has its sections
	REQUIRE(entry_of(b, bin::kUiAnonymous) != 0);
	REQUIRE(entry_of(b, bin::kUiAnonymousDatapoints) != 0);
	REQUIRE(entry_of(b, bin::kUiAnonymousFunctions) != 0);
	for (uint16_t id : {bin::kUiAnonymous, bin::kUiAnonymousDatapoints, bin::kUiAnonymousFunctions}) {
		CHECK(u16(b, entry_of(b, id) + 2) == 0); // optional: an older firmware skips them (section 11)
	}

	// The anonymous body is what the compiler writes as `ui` for a source of just those pages.
	std::string only = cut(cut(fixture("anonymous-pages.json"), "{\"id\": \"garage\", \"label\": {\"de\"", "{\"id\": \"climate\""),
		",\n\t\t{\"id\": \"away\", \"label\"", "\n\t]}\n}");
	hc_test::CompiledImage just;
	REQUIRE(just.compile(only));
	CHECK(section(b, bin::kUiAnonymous) == section(just.bytes, bin::kUi));
	Json body;
	std::string error;
	REQUIRE_MESSAGE(hc_test::parse_json(section(b, bin::kUiAnonymous), body, error), error);
	REQUIRE(body["pages"].items.size() == 2);
	CHECK(body["pages"].items[0]["id"].text == "home");
	CHECK(body["pages"].items[1]["id"].text == "climate");
	CHECK(section(b, bin::kUi).find("\"garage\"") != std::string::npos); // the full set stays in `ui`

	// The datapoints the anonymous pages show, ascending, operated where a control is offered.
	std::vector<hc::DatapointSpec> specs = image.specs();
	auto index = [&](const char* name) {
		for (size_t i = 0; i < specs.size(); ++i) {
			if (std::strcmp(specs[i].name, name) == 0) {
				return static_cast<int>(i);
			}
		}
		FAIL("no datapoint " << name);
		return -1;
	};
	struct Expected {
		const char* name;
		bool operate;
	};
	std::vector<Expected> expected = {
		{"living.light", true},        // a role of the function element
		{"living.light.state", true},  // all its roles
		{"1/3/1", false},              // readOnly, and a reference line
		{"1/3/5", false},              // a chart series
		{"valve", false},              // readOnly on the anonymous page (operated on an operator page only)
		{"rule/heat/setpoint", true},  // a rule parameter by its datapoint
	};
	std::sort(expected.begin(), expected.end(), [&](const Expected& x, const Expected& y) { return index(x.name) < index(y.name); });
	const bin::Table& t = image.view.table(bin::kUiAnonymousDatapoints);
	REQUIRE(t.count == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		CAPTURE(expected[i].name);
		const auto& r = image.view.at<bin::AnonymousDatapointRec>(bin::kUiAnonymousDatapoints, i);
		CHECK(r.datapoint == index(expected[i].name));
		CHECK(((r.flags & bin::kAnonymousOperate) != 0) == expected[i].operate);
	}
	bool operate = true;
	CHECK_FALSE(image.view.anonymous_datapoint(static_cast<size_t>(index("garage.door")), operate));
	CHECK_FALSE(image.view.anonymous_datapoint(static_cast<size_t>(index("away")), operate));
	CHECK_FALSE(image.view.anonymous_datapoint(static_cast<size_t>(index("rule/heat/night")), operate));
	CHECK(image.view.anonymous_datapoint(static_cast<size_t>(index("rule/heat/setpoint")), operate));
	CHECK(operate);
	// The functions: only the living room's light (the garage door is on an operator page).
	REQUIRE(image.view.count(bin::kUiAnonymousFunctions) == 1);
	CHECK(image.view.at<uint16_t>(bin::kUiAnonymousFunctions, 0) == 0);
	CHECK(image.view.anonymous_function(0));
	CHECK_FALSE(image.view.anonymous_function(1));
}

TEST_CASE("Anonymous.WithoutAnonymousPagesNoSection") {
	// No page of minimum role anonymous: none of the three sections, format 1.0 as before.
	hc_test::CompiledImage image;
	REQUIRE(image.compile(without_anonymous()));
	CHECK(u16(image.bytes, 6) == 0);
	CHECK(entry_of(image.bytes, bin::kUiAnonymous) == 0);
	CHECK(entry_of(image.bytes, bin::kUiAnonymousDatapoints) == 0);
	CHECK(entry_of(image.bytes, bin::kUiAnonymousFunctions) == 0);
	size_t length = 0;
	CHECK(image.view.ui_anonymous(length) == nullptr);
	bool operate = true;
	CHECK_FALSE(image.view.anonymous_datapoint(0, operate));
	// The committed pages.json (schema 1.1, no minimum role) compiles as before.
	hc_test::CompiledImage pages;
	REQUIRE(pages.compile(fixture("pages.json")));
	CHECK(u16(pages.bytes, 6) == 0);
	CHECK(entry_of(pages.bytes, bin::kUiAnonymous) == 0);
	// A minimum role that is neither anonymous nor operator is no image.
	std::vector<uint8_t> out;
	std::string error;
	std::string guest = replace_all(fixture("anonymous-pages.json"), "\"minimumRole\": \"operator\"", "\"minimumRole\": \"guest\"");
	CHECK_FALSE(hc::test::compile_image(guest.data(), guest.size(), out, &error));
	CHECK(error.find("minimumRole") != std::string::npos);
}

// ---- the device's checks (10.3 to 10.5) -----------------------------------------------------------

TEST_CASE("Anonymous.ValidatorChecksTheTables") {
	std::vector<uint8_t> good = hc_test::compiled(fixture("anonymous-pages.json"));
	{
		hc::image::Report report;
		CHECK_MESSAGE(structure(good, report), hc_test::describe(report));
	}
	size_t dps = entry_of(good, bin::kUiAnonymousDatapoints);
	size_t fns = entry_of(good, bin::kUiAnonymousFunctions);
	size_t pages = entry_of(good, bin::kUiAnonymous);
	size_t first = u32(good, dps + 4);
	auto refused = [](std::vector<uint8_t> bytes, const char* path, const char* reason) {
		fix_crc(bytes);
		hc::image::Report report;
		CHECK_FALSE(structure(bytes, report));
		CHECK_MESSAGE(refused_at(report, path, reason), hc_test::describe(report));
	};
	{
		// Two entries swapped: not ascending (the binary search would miss).
		std::vector<uint8_t> b = good;
		std::swap_ranges(b.begin() + static_cast<long>(first), b.begin() + static_cast<long>(first) + 4,
			b.begin() + static_cast<long>(first) + 4);
		refused(b, "ui.anonymous.datapoints[1].datapoint", "malformed: the anonymous datapoints do not ascend strictly");
	}
	{
		std::vector<uint8_t> b = good;
		put16(b, first + 4 * (u32(good, dps + 8) / 4 - 1), 999);
		refused(b, "ui.anonymous.datapoints[5].datapoint", "malformed: datapoint 999 does not exist (11 datapoints)");
	}
	{
		std::vector<uint8_t> b = good;
		b[first + 2] = 0x02;
		refused(b, "ui.anonymous.datapoints[0].flags", "malformed: unknown bits 0x2 in flags");
	}
	{
		std::vector<uint8_t> b = good;
		put16(b, u32(good, fns + 4), 7);
		refused(b, "ui.anonymous.functions[0].function", "malformed: function 7 does not exist (2 functions)");
	}
	{
		// The tables without the pages: refused, they would let anonymous in without a page.
		std::vector<uint8_t> b = good;
		put16(b, pages, 0x61); // the body becomes an unknown optional section, skipped
		refused(b, "ui.anonymous.datapoints[0]", "malformed: a table of the anonymous pages without the section ui.anonymous");
	}
	{
		std::vector<uint8_t> b = good;
		b[u32(good, pages + 4) + 3] = 0;
		refused(b, "ui.anonymous", "malformed: a NUL at offset 0x3");
	}
}

// ---- the API -------------------------------------------------------------------------------------

TEST_CASE("Anonymous.EndpointMatrixWithTheAnonymousPages") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	for (const Row& row : kMatrix) {
		std::string target = target_of(f, row.target);
		CAPTURE(method_text(row.method));
		CAPTURE(target);
		// The route table and the matrix agree on what is opened to anonymous.
		std::string path = target.substr(0, target.find('?'));
		CHECK((hc::WebApi::minimum_role(row.method, path.c_str()) == Role::anonymous) == row.opened);
		auto r = f.anonymous(row.method, target, row.body);
		CHECK_MESSAGE(r.status == row.status, r.body);
		if (row.code != nullptr) {
			CHECK(r.json["code"].text == row.code);
		}
	}
}

TEST_CASE("Anonymous.EndpointMatrixWithoutTheSection") {
	// No anonymous page (or no image at all): 401 everywhere but the open endpoints, as before #91.
	for (int variant = 0; variant < 3; ++variant) {
		CAPTURE(variant);
		AnonymousFixture f;
		if (variant == 0) {
			f.load(without_anonymous());
		} else if (variant == 1) {
			f.load(fixture("pages.json"));
		}
		for (const Row& row : kMatrix) {
			// The image of variant 0 has the fixture's datapoints; elsewhere any id will do (refused first).
			std::string target = variant == 0 ? target_of(f, row.target)
				: replace_all(replace_all(replace_all(row.target, "{1/3/1}", "0"), "{", ""), "}", "");
			CAPTURE(method_text(row.method));
			CAPTURE(target);
			auto r = f.anonymous(row.method, target, row.body);
			bool open = std::strncmp(row.pattern, "/api/auth/", 10) == 0 && std::strcmp(row.pattern, "/api/auth/sessions/clear") != 0;
			if (open) {
				CHECK(r.status == row.status);
			} else {
				CHECK_MESSAGE(r.status == 401, r.body);
				CHECK(r.json["code"].text == kRequired);
			}
		}
	}
}

TEST_CASE("Anonymous.EveryDocumentedEndpointHasADecision") {
	// A new endpoint in spec/api.md without a row in the matrix fails here: its decision for anonymous
	// is missing (the code denies it by default, the matrix must say so).
	std::set<std::string> rows;
	for (const Row& row : kMatrix) {
		rows.insert(row.pattern);
	}
	std::set<std::string> documented = documented_endpoints();
	CHECK(documented.size() >= 40);
	for (const std::string& path : documented) {
		CHECK_MESSAGE(rows.count(path) == 1, "spec/api.md documents " << path << ": add its row to kMatrix");
	}
}

TEST_CASE("Anonymous.CsrfAndTokens") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	std::string light = std::to_string(f.id("living.light"));
	// An anonymous POST or PUT needs the CSRF header, as one with the cookie does.
	auto r = f.anonymous(HttpMethod::post, "/api/datapoints/" + light + "/command", "{\"value\":true}", false);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "auth.csrf");
	r = f.anonymous(HttpMethod::put, "/api/rules/heat/parameters/setpoint", "{\"value\":22}", false);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "auth.csrf");
	// A refused route stays 401 without the header (no hint that the header would help).
	r = f.anonymous(HttpMethod::post, "/api/system/restart", "{}", false);
	CHECK(r.status == 401);
	// An invalid token is never taken for anonymous.
	r = f.request(HttpMethod::get, "/api/pages", "", "", "Bearer hct_00000000000000000000000000000000");
	CHECK(r.status == 401);
	CHECK(r.json["code"].text == "auth.token_invalid");
	// A stale cookie is anonymous.
	r = f.request(HttpMethod::get, "/api/pages", "", "hc_session=00000000000000000000000000000000");
	CHECK(r.status == 200);
}

TEST_CASE("Anonymous.WhatTheAnswersHold") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	// The pages: the anonymous body unchanged; an administrator the full `ui`.
	auto r = f.get_streamed("/api/pages", "");
	REQUIRE(r.status == 200);
	CHECK(r.body == f.section_text(true));
	r = f.get_streamed("/api/pages", f.cookie);
	CHECK(r.body == f.section_text(false));

	// The datapoints: the listed ones, writable only where operated.
	r = f.anonymous(HttpMethod::get, "/api/datapoints");
	REQUIRE(r.status == 200);
	CHECK(r.json["count"].number == 6);
	CHECK(r.json["next"].kind == Json::Kind::null);
	std::set<std::string> names;
	for (const Json& dp : r.json["datapoints"].items) {
		names.insert(dp["name"].text);
		bool operated = dp["name"].text == "living.light" || dp["name"].text == "living.light.state" ||
			dp["name"].text == "rule/heat/setpoint";
		CAPTURE(dp["name"].text);
		CHECK(dp["writable"].boolean == (operated && dp["name"].text != "living.light.state"));
		if (!operated) {
			CHECK((static_cast<int>(dp["flags"].number) & 2) == 0);
		}
	}
	CHECK(names == std::set<std::string>{"living.light", "living.light.state", "1/3/1", "1/3/5", "valve", "rule/heat/setpoint"});
	r = f.anonymous(HttpMethod::get, "/api/datapoints?from=" + std::to_string(f.id("valve") + 1));
	CHECK(r.json["datapoints"].items.size() == 1); // only the parameter after the valve
	r = f.get("/api/datapoints", f.cookie);
	CHECK(r.json["count"].number == static_cast<double>(f.model.size()));

	// The model: the listed function, its room, the listed datapoints.
	r = f.anonymous(HttpMethod::get, "/api/model");
	REQUIRE(r.status == 200);
	REQUIRE(r.json["functions"].items.size() == 1);
	CHECK(r.json["functions"].items[0]["id"].text == "living-light");
	REQUIRE(r.json["locations"].items.size() == 1);
	CHECK(r.json["locations"].items[0]["id"].text == "living");
	std::set<std::string> model;
	for (const Json& dp : r.json["datapoints"].items) {
		model.insert(dp["name"].text);
		if (dp["name"].text == "1/3/1") {
			CHECK(dp["writable"].boolean == false); // shown read-only
		}
		if (dp["name"].text == "rule/heat/setpoint") {
			CHECK(dp["writable"].boolean == true);
			CHECK(dp["parameter"]["rule"].text == "heat");
		}
	}
	CHECK(model == names);
	r = f.get("/api/model", f.cookie);
	CHECK(r.json["functions"].items.size() == 2);

	// A parameter set without sign-in: the answer is its datapoint, not the rule.
	r = f.anonymous(HttpMethod::put, "/api/rules/heat/parameters/setpoint", "{\"value\":23}");
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json.has("rule"));
	CHECK(r.json["datapoint"]["name"].text == "rule/heat/setpoint");
	CHECK(r.json["datapoint"]["value"].number == 23);
	r = f.request(HttpMethod::put, "/api/rules/heat/parameters/night", "{\"value\":17}", f.cookie);
	CHECK(r.status == 200);
	CHECK(r.json.has("rule"));

	// An administrator reaches what anonymous may not.
	r = f.request(HttpMethod::post, "/api/datapoints/" + std::to_string(f.id("garage.door")) + "/command", "{\"value\":true}", f.cookie);
	CHECK(r.status == 200);
	r = f.get("/api/status", f.cookie);
	CHECK(r.status == 200);
}

TEST_CASE("Anonymous.SessionReportsTheRole") {
	AnonymousFixture f;
	auto r = f.anonymous(HttpMethod::get, "/api/auth/session");
	CHECK(r.json["role"].text == "anonymous");
	CHECK(r.json["anonymous_pages"].boolean == false);
	f.load(fixture("anonymous-pages.json"));
	r = f.anonymous(HttpMethod::get, "/api/auth/session");
	CHECK(r.json["role"].text == "anonymous");
	CHECK(r.json["authenticated"].boolean == false);
	CHECK(r.json["anonymous_pages"].boolean == true);
	r = f.get("/api/auth/session", f.cookie);
	CHECK(r.json["role"].text == "administrator");
	char token[hc::Auth::kTokenText];
	REQUIRE(f.auth.create_token("script", token) == hc::AuthStatus::ok);
	r = f.get("/api/auth/session", "", std::string("Bearer ") + token);
	CHECK(r.json["role"].text == "administrator");
	// A token sees every page and datapoint, as before.
	r = f.get("/api/datapoints", "", std::string("Bearer ") + token);
	CHECK(r.json["count"].number == static_cast<double>(f.model.size()));
	// Another image without anonymous pages takes the grant away at once.
	f.load(without_anonymous());
	r = f.anonymous(HttpMethod::get, "/api/auth/session");
	CHECK(r.json["anonymous_pages"].boolean == false);
	CHECK(f.anonymous(HttpMethod::get, "/api/pages").status == 401);
	CHECK(hc::role_name(Role::operator_) == std::string("operator"));
}

TEST_CASE("Anonymous.MonitorNamesTheCommand") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	auto r = f.anonymous(HttpMethod::post, "/api/datapoints/" + std::to_string(f.id("living.light")) + "/command",
		"{\"value\":true}");
	REQUIRE(r.status == 200);
	r = f.get("/api/monitor", f.cookie);
	bool found = false;
	for (const Json& event : r.json["events"].items) {
		if (event["kind"].text == "command" && event["name"].text == "living.light") {
			CHECK(event["via"].text == "anonymous");
			CHECK(event["origin"].text == "api");
			found = true;
		}
	}
	CHECK(found);
}

// ---- the live feed -------------------------------------------------------------------------------

TEST_CASE("Anonymous.LiveFeedRestrictedToTheListedDatapoints") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	std::set<int> listed = {f.id("living.light"), f.id("living.light.state"), f.id("1/3/1"), f.id("1/3/5"), f.id("valve"),
		f.id("rule/heat/setpoint")};
	// The anonymous client (no cookie) and an administrator's.
	REQUIRE(f.feed.open(1, nullptr, nullptr, nullptr));
	REQUIRE(f.feed.open(2, f.cookie.c_str(), nullptr, nullptr));
	auto hello = f.transport.messages(1, "hello");
	REQUIRE(hello.size() == 1);
	CHECK(hello[0]["authenticated"].boolean == false);
	CHECK(hello[0]["role"].text == "anonymous");
	CHECK(f.transport.messages(2, "hello")[0]["role"].text == "administrator");
	f.send(1, "{\"type\":\"subscribe\"}");
	f.send(2, "{\"type\":\"subscribe\"}");
	f.run();
	// The snapshot: only the listed datapoints, `count` their number.
	auto snapshot = f.transport.messages(1, "datapoints");
	REQUIRE_FALSE(snapshot.empty());
	CHECK(snapshot[0]["count"].number == 6);
	CHECK(snapshot[0]["from"].number == 0);
	CHECK(snapshot.back()["done"].boolean == true);
	CHECK(f.carried(1) == listed);
	CHECK(f.carried(2).size() == f.model.size());
	for (const Json& message : snapshot) {
		for (const Json& dp : message["updates"].items) {
			if (static_cast<int>(dp["id"].number) == f.id("1/3/1")) {
				CHECK(dp["writable"].boolean == false);
			}
		}
	}
	// The status: the image's identity only.
	auto status = f.transport.messages(1, "status");
	REQUIRE_FALSE(status.empty());
	CHECK(status.back()["image"]["project"].text == "anonymous-house");
	CHECK(status.back()["image"]["revision"].number == 1);
	CHECK_FALSE(status.back().has("network"));
	CHECK_FALSE(status.back().has("device"));
	CHECK(f.transport.messages(2, "status").back().has("network"));

	// The updates: an unlisted datapoint's change reaches the administrator only.
	f.transport.sent.clear();
	REQUIRE(f.request(HttpMethod::post, "/api/datapoints/" + std::to_string(f.id("away")) + "/command", "{\"value\":true}",
		f.cookie).status == 200);
	REQUIRE(f.request(HttpMethod::post, "/api/datapoints/" + std::to_string(f.id("living.light")) + "/command",
		"{\"value\":true}", f.cookie).status == 200);
	f.run();
	// (`away` fires the rules: heat writes 1/3/1, which the anonymous page shows; presence the light.)
	CHECK(f.carried(1) == std::set<int>{f.id("living.light"), f.id("1/3/1")});
	CHECK(f.carried(2).count(f.id("away")) == 1);
	CHECK(f.carried(2).count(f.id("living.light")) == 1);

	// Commands: listed and operated; listed read-only; not listed (also by name).
	f.transport.sent.clear();
	f.send(1, "{\"type\":\"command\",\"id\":" + std::to_string(f.id("living.light")) + ",\"value\":false,\"ref\":1}");
	f.send(1, "{\"type\":\"command\",\"id\":" + std::to_string(f.id("valve")) + ",\"value\":10,\"ref\":2}");
	f.send(1, "{\"type\":\"command\",\"id\":" + std::to_string(f.id("garage.door")) + ",\"value\":true,\"ref\":3}");
	f.send(1, "{\"type\":\"command\",\"id\":\"garage.door\",\"value\":true,\"ref\":4}");
	f.send(1, "{\"type\":\"command\",\"id\":\"rule/heat/setpoint\",\"value\":24,\"ref\":5}");
	auto results = f.transport.messages(1, "result");
	REQUIRE(results.size() == 5);
	CHECK(results[0]["ok"].boolean == true);
	CHECK(results[1]["code"].text == "dp.not_writable");
	CHECK(results[2]["code"].text == "dp.unknown_id");
	CHECK(results[3]["code"].text == "dp.unknown_id");
	CHECK(results[4]["ok"].boolean == true);
	CHECK(f.model.find("garage.door")->quality() == hc::Quality::unknown); // never commanded by anonymous

	// The monitor needs a sign-in; the connection stays.
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true}");
	auto errors = f.transport.messages(1, "error");
	REQUIRE(errors.size() == 1);
	CHECK(errors[0]["code"].text == "auth.required");
	CHECK(f.transport.messages(1, "monitor").empty());
	CHECK(f.feed.client_count() == 2);
	// No timeout: anonymous is a role, not a client waiting for its token.
	f.advance(hc::LiveFeed::kAuthTimeoutMs + 2000);
	CHECK(f.feed.client_count() == 2);
	CHECK(f.transport.closes.empty());
}

TEST_CASE("Anonymous.LiveFeedClosesWhenTheGrantGoes") {
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	REQUIRE(f.feed.open(1, nullptr, nullptr, nullptr));
	REQUIRE(f.feed.open(2, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	f.run();
	// Another image with anonymous pages: the anonymous client stays and gets its new snapshot.
	f.load(fixture("anonymous-pages.json"));
	f.advance(1500);
	CHECK(f.feed.client_count() == 2);
	// An image without them: the anonymous client is closed within a second, the session stays.
	f.load(without_anonymous());
	f.advance(1500);
	CHECK(f.feed.client_count() == 1);
	REQUIRE(f.transport.closes.size() == 1);
	CHECK(f.transport.closes[0].client == 1);
	CHECK(f.transport.closes[0].code == hc::websocket::kClosePolicy);
	CHECK(f.transport.messages(1, "error").back()["code"].text == "auth.required");
	// Without the grant a client without a cookie waits for a token, as before.
	REQUIRE(f.feed.open(3, nullptr, nullptr, nullptr));
	CHECK(f.transport.messages(3, "hello")[0]["role"].kind == Json::Kind::null);
	f.send(3, "{\"type\":\"subscribe\"}");
	CHECK(f.transport.messages(3, "error")[0]["code"].text == "auth.required");
}

TEST_CASE("Anonymous.LiveFeedTokenUpgrade") {
	// An anonymous client that authenticates with a token sees everything from then on.
	AnonymousFixture f;
	f.load(fixture("anonymous-pages.json"));
	char token[hc::Auth::kTokenText];
	REQUIRE(f.auth.create_token("script", token) == hc::AuthStatus::ok);
	REQUIRE(f.feed.open(1, nullptr, nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	f.run();
	CHECK(f.carried(1).size() == 6);
	f.send(1, std::string("{\"type\":\"auth\",\"token\":\"") + token + "\"}");
	CHECK(f.transport.messages(1, "auth")[0]["ok"].boolean == true);
	f.run();
	CHECK(f.carried(1).size() == f.model.size());
	f.send(1, "{\"type\":\"command\",\"id\":\"garage.door\",\"value\":true,\"ref\":1}");
	CHECK(f.transport.messages(1, "result").back()["ok"].boolean == true);
}
