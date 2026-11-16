/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The control pages of the runtime image (issue #54, spec/runtime-image.md 3.9; binary since issue
// #87, 9.16): the image's `ui` section, which the compiler writes from the source's `ui.pages`, is
// served unchanged by GET /api/pages (spec/api.md: five flat lists, the defaults resolved, the
// datapoints by their index, one response without paging); the fixtures spec/fixtures/images/pages.json
// and ehome-72-pages.json, the limits of section 5 in one response, the images without pages, and the
// datapoint metadata the page controls use (ranges, choices of /api/model; the history's store and
// mean).

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
	size_t at = text.find(from);
	REQUIRE_MESSAGE(at != std::string::npos, "not in the text: " << from);
	return text.replace(at, from.size(), to);
}

/// The API with the image service (as the dashboard tests of web_api_test.cpp).
struct PagesFixture : WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot;
	hc::image::ImageService service{controller, model, blobs, slot, adapter, &tunnel};
	std::string cookie;

	PagesFixture() {
		fake.clock.wall_seconds = 1792231392; // 2026-10-17
		fake.clock.synchronized = true;
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	/// Compiles the source `text` and deploys it.
	void load(const std::string& text) {
		REQUIRE_MESSAGE(hc_test::deploy(service, text), hc_test::describe(service.report()));
	}

	int id(const char* name) const {
		const hc::Datapoint* dp = model.find(name);
		REQUIRE_MESSAGE(dp != nullptr, "no datapoint " << name);
		return dp->id();
	}

	/// What the compiler wrote into the loaded image's `ui` section ("" for none).
	std::string ui() const {
		size_t length = 0;
		const char* text = service.image().ui(length);
		return text != nullptr ? std::string(text, length) : std::string();
	}

	/// GET /api/pages, read whole as a platform does (streamed when larger than the buffer); the
	/// body must be the `ui` section byte for byte, in one response (no paging members).
	hc_test::Json pages(bool* streamed = nullptr) {
		Result r = get_streamed("/api/pages", cookie, streamed);
		REQUIRE_MESSAGE(r.status == 200, r.body);
		CHECK(r.body == ui());
		for (const char* gone : {"loaded", "generation", "total", "next", "from"}) {
			CHECK_MESSAGE(!r.json.has(gone), "GET /api/pages has no paging member " << gone);
		}
		return r.json;
	}
};

/// An image with `pages` pages of `sections` sections of `elements` elements each; every element a
/// chart of `series` series and `references` reference lines when `series` > 0, else a datapoint.
std::string generated(size_t pages, size_t sections, size_t elements, size_t series = 0, size_t references = 0) {
	std::string text = R"({"image": {"schemaVersion": "1.1", "projectId": "gen", "revision": 1},
		"datapoints": [{"id": "t", "label": "T", "type": "real", "unit": "°C", "history": {"retention": 3600, "spacing": 60}}],
		"ui": {"pages": [)";
	for (size_t p = 0; p < pages; ++p) {
		text += std::string(p > 0 ? "," : "") + "{\"id\": \"p" + std::to_string(p) + "\", \"label\": \"Page\", \"sections\": [";
		for (size_t s = 0; s < sections; ++s) {
			text += std::string(s > 0 ? "," : "") + "{\"label\": \"Section\", \"elements\": [";
			for (size_t e = 0; e < elements; ++e) {
				text += e > 0 ? "," : "";
				if (series == 0) {
					text += "{\"datapoint\": \"t\", \"label\": \"Value\"}";
					continue;
				}
				text += "{\"chart\": {\"label\": \"Chart\", \"series\": [";
				for (size_t k = 0; k < series; ++k) {
					text += std::string(k > 0 ? "," : "") + "{\"datapoint\": \"t\", \"label\": \"Series\"}";
				}
				text += "], \"references\": [";
				for (size_t k = 0; k < references; ++k) {
					text += std::string(k > 0 ? "," : "") + "{\"value\": 21, \"label\": \"Limit\"}";
				}
				text += "]}}";
			}
			text += "]}";
		}
		text += "]}";
	}
	return text + "]}}";
}

} // namespace

TEST_CASE("Pages.FixtureCompilesAndLoads") {
	// pages.json (schema 1.1) and the same source as a newer schema compile into images whose `ui`
	// section the device takes; the image's schema version travels as text.
	PagesFixture f;
	f.load(fixture("pages.json"));
	CHECK(std::string(f.service.info().schema_version) == "1.1");
	CHECK_FALSE(f.ui().empty());
	f.load(replace(fixture("pages.json"), "\"1.1\"", "\"1.7\""));
	CHECK(std::string(f.service.info().schema_version) == "1.7");
	CHECK_FALSE(f.ui().empty());
}

TEST_CASE("Pages.UiSectionAsTheCompilerWroteIt") {
	// The section is the finished body (spec/runtime-image.md 9.16): the defaults resolved, the
	// functions by their ids, the datapoints by their index in the image (the API's id).
	PagesFixture f;
	f.load(fixture("pages.json"));
	hc_test::Json ui;
	std::string error;
	REQUIRE_MESSAGE(hc_test::parse_json(f.ui(), ui, error), error << "\n" << f.ui());
	REQUIRE(ui["pages"].items.size() == 2);
	REQUIRE(ui["sections"].items.size() == 3);
	REQUIRE(ui["elements"].items.size() == 9);
	REQUIRE(ui["series"].items.size() == 4);
	REQUIRE(ui["references"].items.size() == 2);
	CHECK(ui["pages"].items[0]["id"].text == "evening");
	CHECK(ui["pages"].items[0]["icon"].text == "moon");
	CHECK(ui["pages"].items[0]["label"]["de"].text == "Abend");
	CHECK(ui["pages"].items[0]["label"]["en"].text == "Evening");
	CHECK(ui["pages"].items[0]["sections"].number == 2);
	CHECK_FALSE(ui["pages"].items[1].has("icon"));
	CHECK_FALSE(ui["sections"].items[1].has("label"));
	CHECK(ui["sections"].items[2]["page"].number == 1);
	// Elements: the function by its id, the datapoint by its model id, readOnly kept.
	const auto& elements = ui["elements"].items;
	CHECK(elements[0]["function"].text == "living-light");
	CHECK(elements[0]["label"]["de"].text == "Decke");
	CHECK(elements[2]["datapoint"].number == f.id("1/3/1"));
	CHECK(elements[2]["readOnly"].boolean);
	CHECK(elements[3]["datapoint"].number == f.id("rule/heat/setpoint"));
	CHECK_FALSE(elements[3].has("readOnly"));
	CHECK(elements[4]["datapoint"].number == f.id("rule/presence/enabled"));
	// The chart and its series and reference lines.
	const auto& chart = elements[5]["chart"];
	CHECK(chart["width"].text == "full");
	CHECK(chart["height"].number == 4);
	CHECK(chart["range"].text == "24h");
	REQUIRE(chart["presets"].items.size() == 4);
	CHECK(chart["presets"].items[0].text == "24h");
	CHECK(chart["presets"].items[3].text == "1y");
	CHECK(chart["axes"]["left"]["min"].kind == hc_test::Json::Kind::null);
	CHECK(chart["axes"]["left"]["max"].kind == hc_test::Json::Kind::null);
	CHECK(chart["axes"]["right"]["min"].number == 0);
	CHECK(chart["axes"]["right"]["max"].number == 100);
	const auto& series = ui["series"].items;
	CHECK(series[0]["datapoint"].number == f.id("1/3/1"));
	CHECK(series[0]["element"].number == 5);
	CHECK(series[1]["axis"].text == "right");
	CHECK(series[2]["consolidation"].text == "mean"); // a boolean's default: the on-share
	CHECK(series[3]["consolidation"].text == "consumption");
	CHECK(ui["references"].items[0]["value"].number == 21);
	CHECK_FALSE(ui["references"].items[0].has("datapoint"));
	CHECK(ui["references"].items[1]["datapoint"].number == f.id("rule/heat/setpoint"));
	CHECK_FALSE(ui["references"].items[1].has("value"));
	// The second chart's defaults: height 2, its range and presets as given, both axes automatic.
	const auto& meter = elements[8]["chart"];
	CHECK(meter["width"].number == 2);
	CHECK(meter["height"].number == 2);
	CHECK(meter["range"].text == "7d");
	REQUIRE(meter["presets"].items.size() == 2);
	CHECK(meter["presets"].items[0].text == "7d");
	CHECK(meter["presets"].items[1].text == "30d");
	CHECK(meter["axes"]["left"]["min"].kind == hc_test::Json::Kind::null);
	CHECK(meter["axes"]["right"]["max"].kind == hc_test::Json::Kind::null);
	CHECK(series[3]["axis"].text == "left");
	// The section is read in place from the slot (spec/runtime-image.md I2): no copy in RAM.
	size_t length = 0;
	const char* text = f.service.image().ui(length);
	CHECK(reinterpret_cast<const uint8_t*>(text) >= f.slot.bytes());
	CHECK(reinterpret_cast<const uint8_t*>(text) + length <= f.slot.bytes() + f.service.info().bytes);
}

// The ui.pages E-Home exports for the amendment's acceptance case (ehome#72), verbatim in the fixture.
TEST_CASE("Pages.EHomeExportLoads") {
	PagesFixture f;
	f.load(fixture("ehome-72-pages.json"));
	hc_test::Json all = f.pages();
	REQUIRE(all["pages"].items.size() == 2);
	REQUIRE(all["sections"].items.size() == 3);
	REQUIRE(all["elements"].items.size() == 10);
	REQUIRE(all["series"].items.size() == 3);
	REQUIRE(all["references"].items.size() == 2);
	const auto& elements = all["elements"].items;
	CHECK(elements[3]["function"].text == "kitchen-light");
	CHECK(elements[4]["datapoint"].number == f.id("1/0/1"));
	CHECK(elements[4]["readOnly"].boolean);
	CHECK(elements[5]["datapoint"].number == f.id("holiday"));
	CHECK(elements[6]["datapoint"].number == f.id("rule/heat/setpoint"));
	CHECK(elements[7]["datapoint"].number == f.id("rule/heat/enabled"));
	CHECK(elements[7]["label"]["de"].text == "Heizregel");
	CHECK(elements[8].has("chart"));
	CHECK(elements[8]["chart"]["label"]["de"].text == "Temperaturen");
	CHECK(elements[9]["function"].text == "kitchen-light");
	CHECK(all["series"].items[2]["consolidation"].text == "mean"); // the boolean's on-share

	// A chart with an empty series array loads (E-Home drops such a chart, the device accepts it).
	std::string text = fixture("ehome-72-pages.json");
	size_t from = text.find("\"series\": [{\"datapoint\": \"1/3/1\"");
	size_t to = text.find("\"references\"", from);
	REQUIRE((from != std::string::npos && to != std::string::npos));
	text.replace(from, to - from, "\"series\": [],\n   ");
	f.load(text);
	all = f.pages();
	CHECK(all["elements"].items[8].has("chart"));
	CHECK(all["series"].items.empty());
}

TEST_CASE("Pages.ApiServesThePagesToTheUi") {
	PagesFixture f;
	f.load(fixture("pages.json"));
	auto r = f.get("/api/pages", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	// The section as the compiler wrote it, byte for byte, in one response (spec/api.md).
	CHECK(r.body == f.ui());
	CHECK_FALSE(r.json.has("total"));
	CHECK_FALSE(r.json.has("next"));
	CHECK_FALSE(r.json.has("generation"));
	auto& pages = r.json["pages"].items;
	REQUIRE(pages.size() == 2);
	CHECK(pages[0]["id"].text == "evening");
	CHECK(pages[0]["label"]["de"].text == "Abend");
	CHECK(pages[0]["icon"].text == "moon");
	CHECK(pages[0]["sections"].number == 2);
	CHECK(pages[1]["label"].text == "Energy");
	auto& sections = r.json["sections"].items;
	REQUIRE(sections.size() == 3);
	CHECK(sections[0]["page"].number == 0);
	CHECK(sections[0]["label"]["de"].text == "Wohnzimmer");
	CHECK(sections[0]["elements"].number == 5);
	CHECK(sections[1].members.count("label") == 0);
	auto& elements = r.json["elements"].items;
	REQUIRE(elements.size() == 9);
	CHECK(elements[0]["function"].text == "living-light");
	CHECK(elements[0]["label"]["de"].text == "Decke");
	CHECK(elements[1]["function"].text == "evening");
	CHECK(elements[2]["datapoint"].number == f.id("1/3/1"));
	CHECK(elements[2]["readOnly"].boolean);
	CHECK(elements[3]["datapoint"].number == f.id("rule/heat/setpoint"));
	CHECK(elements[3].members.count("readOnly") == 0);
	auto& chart = elements[5]["chart"];
	CHECK(elements[5]["section"].number == 1);
	CHECK(chart["label"]["de"].text == "Temperaturen");
	CHECK(chart["width"].text == "full");
	CHECK(chart["height"].number == 4);
	CHECK(chart["range"].text == "24h");
	REQUIRE(chart["presets"].items.size() == 4);
	CHECK(chart["presets"].items[3].text == "1y");
	CHECK(chart["axes"]["left"]["min"].kind == hc_test::Json::Kind::null);
	CHECK(chart["axes"]["right"]["max"].number == 100);
	CHECK(elements[8]["chart"]["width"].number == 2);
	auto& series = r.json["series"].items;
	REQUIRE(series.size() == 4);
	CHECK(series[0]["element"].number == 5);
	CHECK(series[0]["datapoint"].number == f.id("1/3/1"));
	CHECK(series[0]["label"]["de"].text == "Wohnzimmer");
	CHECK(series[0]["axis"].text == "left");
	CHECK(series[1]["axis"].text == "right");
	CHECK(series[2]["consolidation"].text == "mean");
	CHECK(series[3]["consolidation"].text == "consumption");
	CHECK(series[3]["element"].number == 8);
	auto& references = r.json["references"].items;
	REQUIRE(references.size() == 2);
	CHECK(references[0]["value"].number == 21);
	CHECK(references[0]["label"]["de"].text == "Soll");
	CHECK(references[1]["datapoint"].number == f.id("rule/heat/setpoint"));
	CHECK(references[1].members.count("value") == 0);
	// The query of firmware up to 1.2 changes nothing: the whole answer.
	CHECK(f.get("/api/pages?from=14", f.cookie).body == r.body);

	CHECK(f.request(HttpMethod::post, "/api/pages", "{}", f.cookie).status == 405);
	CHECK(f.get("/api/pages").status == 401);
}

TEST_CASE("Pages.ApiPagesAtTheLimitsInOneResponse") {
	PagesFixture f;
	// 16 pages of 8 sections of 4 datapoint elements (512): larger than the response buffer, streamed
	// as one body.
	f.load(generated(16, 8, 4));
	bool streamed = false;
	hc_test::Json all = f.pages(&streamed);
	CHECK(streamed);
	CHECK(all["pages"].items.size() == 16);
	CHECK(all["sections"].items.size() == 128);
	CHECK(all["elements"].items.size() == 512);
	CHECK(all["elements"].items[511]["section"].number == 127);
	CHECK(all["sections"].items[127]["page"].number == 15);
	// Series and reference lines at their limits: 128 charts of 2 series and one line.
	f.load(generated(16, 8, 1, 2, 1));
	all = f.pages();
	CHECK(all["series"].items.size() == 256);
	CHECK(all["references"].items.size() == 128);
	CHECK(all["series"].items[255]["element"].number == 127);
	// A stream of an image that has since been replaced breaks off (the token changed).
	hc::WebRequest req;
	req.method = hc::HttpMethod::get;
	req.path = "/api/pages";
	req.query = "";
	req.cookie = f.cookie.c_str();
	req.requested_with = "HomeControl";
	req.source = "192.0.2.7";
	hc::WebResponse resp(f.buffer, sizeof(f.buffer));
	f.api.handle(req, resp);
	REQUIRE(resp.stream);
	f.load(generated(16, 8, 4));
	char piece[100];
	size_t length = 0;
	CHECK_FALSE(f.api.read_stream(resp.stream_token, 0, piece, sizeof(piece), length));
}

TEST_CASE("Pages.WithoutPagesTheListsAreEmpty") {
	PagesFixture f;
	auto r = f.get("/api/pages", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.body == "{\"loaded\":false}");
	// An image of 1.0 with `ui: {}` loads, keeps its model, and has no pages: every list empty, the
	// UI falls back to the generated overview.
	f.load(fixture("example.json"));
	r = f.get("/api/pages", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.body == "{\"pages\":[],\"sections\":[],\"elements\":[],\"series\":[],\"references\":[]}");
	CHECK(f.get("/api/model", f.cookie).json["functions"].items.size() == 9);
	// A swap to an image with pages and back: the lists follow the image.
	f.load(fixture("pages.json"));
	CHECK(f.get("/api/pages", f.cookie).json["pages"].items.size() == 2);
	f.load(fixture("minimal.json"));
	CHECK(f.get("/api/pages", f.cookie).json["pages"].items.empty());
	// Unloaded: no image again.
	REQUIRE(f.request(HttpMethod::del, "/api/image", "", f.cookie).status == 200);
	CHECK(f.get("/api/pages", f.cookie).body == "{\"loaded\":false}");
}

TEST_CASE("Pages.ModelGivesTheControlsTheirRanges") {
	PagesFixture f;
	f.load(fixture("pages.json"));
	std::map<std::string, hc_test::Json> byName;
	uint32_t from = 0;
	for (int guard = 0; guard < 20; ++guard) {
		auto r = f.get("/api/model?from=" + std::to_string(from), f.cookie);
		REQUIRE(r.status == 200);
		for (const auto& dp : r.json["datapoints"].items) {
			byName[dp["name"].text] = dp;
		}
		if (r.json["next"].kind != hc_test::Json::Kind::number) {
			break;
		}
		from = static_cast<uint32_t>(r.json["next"].number);
	}
	// A rule parameter: its min, max and step.
	hc_test::Json& setpoint = byName["rule/heat/setpoint"];
	CHECK(setpoint["writable"].boolean);
	CHECK(setpoint["min"].number == 15);
	CHECK(setpoint["max"].number == 26);
	CHECK(setpoint["step"].number == 0.5);
	CHECK(setpoint["unit"].text == "°C");
	// The enable switch: a boolean, labelled with the rule's name.
	hc_test::Json& enabled = byName["rule/presence/enabled"];
	CHECK(enabled["type"].text == "boolean");
	CHECK(enabled["writable"].boolean);
	CHECK(enabled["label"].text == "Presence");
	CHECK(enabled.members.count("min") == 0);
	// A scaled KNX type: its fixed range.
	CHECK(byName["valve"]["min"].number == 0);
	CHECK(byName["valve"]["max"].number == 100);
	CHECK(byName["1/3/1"].members.count("min") == 0);
}

TEST_CASE("Pages.ElementDatapointsAreCommandedLikeAnyOther") {
	// Rule parameters and the enable switch need no special case (spec/rules.md 8.6): the page's
	// controls command their datapoints; a refusal comes back with its reason.
	PagesFixture f;
	f.load(fixture("pages.json"));
	auto r = f.post("/api/datapoints/" + std::to_string(f.id("rule/heat/setpoint")) + "/command", "{\"value\": 22.5}",
		f.cookie);
	CHECK_MESSAGE(r.status == 200, r.body);
	f.controller.run_once();
	auto rules = f.get("/api/rules", f.cookie);
	REQUIRE(rules.status == 200);
	CHECK(rules.json["rules"].items[0]["parameters"].items[0]["value"].number == 22.5);
	r = f.post("/api/datapoints/" + std::to_string(f.id("rule/presence/enabled")) + "/command", "{\"value\": false}",
		f.cookie);
	CHECK_MESSAGE(r.status == 200, r.body);
	f.controller.run_once();
	rules = f.get("/api/rules", f.cookie);
	CHECK_FALSE(rules.json["rules"].items[1]["enabled"].boolean);
	// Outside the parameter's range: refused with the reason the tile shows.
	r = f.post("/api/datapoints/" + std::to_string(f.id("rule/heat/setpoint")) + "/command", "{\"value\": 40}", f.cookie);
	CHECK(r.status >= 400);
	CHECK_FALSE(r.json["error"].text.empty());
	// A datapoint that is not writable stays read-only, whatever the element says.
	r = f.post("/api/datapoints/" + std::to_string(f.id("1/3/5")) + "/command", "{\"value\": 40}", f.cookie);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "dp.not_writable");
}

TEST_CASE("Pages.ConsumptionAndOnShareFromTheRawRing") {
	// Without archives the chart queries the raw ring: a bucket's last value
	// gives a consumption bar, a boolean's mean the on-share; the response names the store and its
	// reach, from which the UI says "only the last ... stored".
	PagesFixture f;
	f.load(replace(replace(fixture("pages.json"), "\"listen\": [\"1/5/1\"]}}", "\"listen\": [\"1/5/1\"]}, \"writable\": true}"),
		"\"type\": \"boolean\",\n\t\t \"history\"", "\"type\": \"boolean\", \"writable\": true,\n\t\t \"history\""));
	f.controller.run_once();
	int meter = f.id("meter");
	int heating = f.id("1/1/7");
	for (int hour = 0; hour < 48; ++hour) {
		f.fake.clock.wall_seconds += 3600;
		f.fake.clock.advance(3600000);
		auto c = f.post("/api/datapoints/" + std::to_string(meter) + "/command", "{\"value\": " + std::to_string(1000 + hour * 100) + "}",
			f.cookie);
		REQUIRE_MESSAGE(c.status == 200, c.body);
		c = f.post("/api/datapoints/" + std::to_string(heating) + "/command", hour % 4 == 0 ? "{\"value\": true}" : "{\"value\": false}",
			f.cookie);
		REQUIRE_MESSAGE(c.status == 200, c.body);
		f.controller.run_once();
	}
	int64_t now = f.fake.clock.wall_seconds;
	int64_t from = now - 2 * 86400;
	auto r = f.get("/api/history?ids=" + std::to_string(meter) + "," + std::to_string(heating) + "&from=" +
		std::to_string(from) + "&to=" + std::to_string(now + 1) + "&points=2", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto& m = r.json["series"].items[0];
	CHECK(m["store"].text == "raw");
	CHECK(m["retention_s"].number == 604800);
	REQUIRE(m["points"].items.size() == 2);
	double day1 = m["points"].items[0].items[1].number;
	double day2 = m["points"].items[1].items[1].number;
	CHECK(day2 - day1 == doctest::Approx(2400)); // 24 hours of 100 Wh
	auto& h = r.json["series"].items[1];
	CHECK(h["retention_s"].number == 86400);
	REQUIRE(!h["points"].items.empty());
	auto& bucket = h["points"].items.back();
	// A boolean's bucket carries on_s, its seconds on, beside the on-share (issue #56).
	REQUIRE(bucket.items.size() == 7);
	CHECK(bucket.items[5].number > 0);
	CHECK(bucket.items[5].number < 0.5); // on for about one sample in four
	CHECK(bucket.items[6].number > 0);
}
