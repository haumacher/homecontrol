/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The control pages of the runtime image (issue #54, schema 1.1, spec/runtime-image.md 3.9): the
// fixture spec/fixtures/images/pages.json loads, its tables in the UI model, GET /api/pages, the
// limits of section 5 and what they cost, the 1.0 images that still load, and the datapoint
// metadata the page controls use (ranges, choices of /api/model; the history's store and mean).

#include <cstring>
#include <map>
#include <string>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/image/loader.h"
#include "hc/image/model_view.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;
using hc::image::Consolidation;
using hc::image::Loader;
using hc::image::ModelView;
using hc::image::Plan;
using hc::image::Report;
using hc_test::WebFixture;

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

std::string describe(const Report& report) {
	std::string out = std::string(hc::image::report_kind_name(report.kind)) + ":";
	for (size_t i = 0; i < report.count; ++i) {
		out += std::string("\n  ") + report.refusals[i].path + ": " + report.refusals[i].reason;
	}
	return out;
}

bool parse(const std::string& text, Plan& plan, Report& report) {
	hc_test::FakeClock clock;
	Loader loader(clock, hc::HistoryLimits().cap_bytes);
	return loader.parse(text.data(), text.size(), plan, report);
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
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	std::string cookie;

	PagesFixture() {
		fake.clock.wall_seconds = 1792231392; // 2026-10-17
		fake.clock.synchronized = true;
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void load(const std::string& text) {
		REQUIRE(service.load(text.data(), text.size(), hc::image::Source::api, true));
	}

	int id(const char* name) const {
		const hc::Datapoint* dp = model.find(name);
		REQUIRE_MESSAGE(dp != nullptr, "no datapoint " << name);
		return dp->id();
	}

	/// Every page of GET /api/pages, the lists concatenated.
	hc_test::Json pages() {
		hc_test::Json all;
		uint32_t from = 0;
		for (int guard = 0; guard < 100; ++guard) {
			Result r = get("/api/pages?from=" + std::to_string(from), cookie);
			REQUIRE_MESSAGE(r.status == 200, r.body);
			if (guard == 0) {
				all = r.json;
			} else {
				for (const char* list : {"pages", "sections", "elements", "series", "references"}) {
					for (const hc_test::Json& item : r.json[list].items) {
						all.members[list].items.push_back(item);
					}
				}
			}
			if (r.json["next"].kind != hc_test::Json::Kind::number) {
				return all;
			}
			from = static_cast<uint32_t>(r.json["next"].number);
		}
		FAIL("GET /api/pages does not end");
		return all;
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

void refused_at(const std::string& text, const char* path, const char* reason) {
	Plan plan;
	Report report;
	CHECK_FALSE(parse(text, plan, report));
	bool found = false;
	for (size_t i = 0; i < report.count && !found; ++i) {
		found = std::string(report.refusals[i].path) == path && std::strstr(report.refusals[i].reason, reason) != nullptr;
	}
	CHECK_MESSAGE(found, "expected " << path << ": ..." << reason << "...\n" << describe(report));
}

} // namespace

TEST_CASE("Pages.FixtureLoadsAsSchema11") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(fixture("pages.json"), plan, report), describe(report));
	CHECK(std::string(plan.info.schema_version) == "1.1");
	// A newer minor loads (I4); the major refusal names what this firmware reads.
	REQUIRE_MESSAGE(parse(replace(fixture("pages.json"), "\"1.1\"", "\"1.7\""), plan, report), describe(report));
	CHECK_FALSE(parse(replace(fixture("pages.json"), "\"1.1\"", "\"2.0\""), plan, report));
	CHECK(report.kind == Report::Kind::version);
	CHECK(std::strstr(report.refusals[0].reason, (std::string("(1.x, up to ") + hc::image::schema_version_text() + ")").c_str()) !=
		nullptr);
}

TEST_CASE("Pages.TablesOfTheUiModel") {
	PagesFixture f;
	f.load(fixture("pages.json"));
	const ModelView& view = f.service.view();
	REQUIRE(view.page_count() == 2);
	REQUIRE(view.section_count() == 3);
	REQUIRE(view.element_count() == 9);
	REQUIRE(view.chart_count() == 2);
	REQUIRE(view.series_count() == 4);
	REQUIRE(view.reference_count() == 2);
	CHECK(std::string(view.page(0).id) == "evening");
	CHECK(std::string(view.page(0).icon) == "moon");
	CHECK(std::string(view.page(0).label) == R"({"de": "Abend", "en": "Evening"})");
	CHECK(view.page(0).sections == 2);
	CHECK(view.page(1).icon == nullptr);
	CHECK(view.section(1).label == nullptr);
	CHECK(view.section(2).page == 1);
	// Elements: the function by its index, the datapoint by its model id, readOnly kept.
	const ModelView::Element& light = view.element(0);
	CHECK(light.kind == ModelView::ElementKind::function);
	CHECK(std::string(view.function(light.target).id) == "living-light");
	CHECK(std::string(light.label) == R"({"de": "Decke"})");
	const ModelView::Element& temperature = view.element(2);
	CHECK(temperature.kind == ModelView::ElementKind::datapoint);
	CHECK(temperature.target == f.id("1/3/1"));
	CHECK(temperature.read_only);
	CHECK(view.element(3).target == f.id("rule/heat/setpoint"));
	CHECK_FALSE(view.element(3).read_only);
	CHECK(view.element(4).target == f.id("rule/presence/enabled"));
	// The chart and its series and reference lines.
	const ModelView::Element& element = view.element(5);
	REQUIRE(element.kind == ModelView::ElementKind::chart);
	const ModelView::Chart& chart = view.chart(element.target);
	CHECK(chart.width == 0);
	CHECK(chart.height == 4);
	CHECK(std::string(hc::image::chart_range_name(chart.range)) == "24h");
	CHECK(chart.presets == 0x1E); // 24h, 7d, 30d, 1y
	CHECK(chart.fixed == 0x0C);   // the right axis 0..100
	CHECK(chart.min[1] == 0);
	CHECK(chart.max[1] == 100);
	CHECK(view.series(0).datapoint == f.id("1/3/1"));
	CHECK(view.series(0).element == 5);
	CHECK(view.series(1).axis == 1);
	CHECK(view.series(2).consolidation == Consolidation::mean); // a boolean's default: the on-share
	CHECK(view.series(3).consolidation == Consolidation::consumption);
	CHECK(view.reference(0).constant);
	CHECK(view.reference(0).value == 21);
	CHECK_FALSE(view.reference(1).constant);
	CHECK(view.reference(1).datapoint == f.id("rule/heat/setpoint"));
	// The second chart's defaults: height 2, its range alone when no presets were given would be the range.
	const ModelView::Chart& meter = view.chart(view.element(8).target);
	CHECK(meter.width == 2);
	CHECK(meter.height == 2);
	CHECK(std::string(hc::image::chart_range_name(meter.range)) == "7d");
	CHECK(meter.presets == 0x0C);
}

// The ui.pages E-Home exports for the amendment's acceptance case (ehome#72), verbatim in the fixture.
TEST_CASE("Pages.EHomeExportLoads") {
	PagesFixture f;
	f.load(fixture("ehome-72-pages.json"));
	const ModelView& view = f.service.view();
	REQUIRE(view.page_count() == 2);
	REQUIRE(view.section_count() == 3);
	REQUIRE(view.element_count() == 10);
	REQUIRE(view.chart_count() == 1);
	REQUIRE(view.series_count() == 3);
	REQUIRE(view.reference_count() == 2);
	CHECK(std::string(view.function(view.element(3).target).id) == "kitchen-light");
	CHECK(view.element(4).target == f.id("1/0/1"));
	CHECK(view.element(4).read_only);
	CHECK(view.element(5).target == f.id("holiday"));
	CHECK(view.element(6).target == f.id("rule/heat/setpoint"));
	CHECK(view.element(7).target == f.id("rule/heat/enabled"));
	CHECK(view.element(8).kind == ModelView::ElementKind::chart);
	CHECK(view.element(9).kind == ModelView::ElementKind::function);
	hc_test::Json all = f.pages();
	CHECK(all["total"].number == 2 + 3 + 10 + 3 + 2);
	CHECK(all["elements"].items[7]["label"]["de"].text == "Heizregel");
	CHECK(all["elements"].items[8]["chart"]["label"]["de"].text == "Temperaturen");
	CHECK(all["series"].items[2]["consolidation"].text == "mean"); // the boolean's on-share

	// A chart with an empty series array loads (E-Home drops such a chart, the device accepts it).
	std::string text = fixture("ehome-72-pages.json");
	size_t from = text.find("\"series\": [{\"datapoint\": \"1/3/1\"");
	size_t to = text.find("\"references\"", from);
	REQUIRE((from != std::string::npos && to != std::string::npos));
	text.replace(from, to - from, "\"series\": [],\n   ");
	f.load(text);
	CHECK(f.service.view().chart_count() == 1);
	CHECK(f.service.view().series_count() == 0);
}

TEST_CASE("Pages.ApiServesThePagesToTheUi") {
	PagesFixture f;
	f.load(fixture("pages.json"));
	auto r = f.get("/api/pages", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["total"].number == 2 + 3 + 9 + 4 + 2);
	CHECK(r.json["next"].kind == hc_test::Json::Kind::null);
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

	// Paging: from= continues in the shared index space.
	r = f.get("/api/pages?from=14", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["pages"].items.empty());
	CHECK(r.json["elements"].items.empty());
	CHECK(r.json["series"].items.size() == 4);
	CHECK(r.json["series"].items[0]["datapoint"].number == f.id("1/3/1"));

	CHECK(f.request(HttpMethod::post, "/api/pages", "{}", f.cookie).status == 405);
	CHECK(f.get("/api/pages").status == 401);
}

TEST_CASE("Pages.ApiPagesAtTheLimitsSpanSeveralResponses") {
	PagesFixture f;
	// 16 pages of 8 sections of 4 datapoint elements (512): more than one response holds.
	f.load(generated(16, 8, 4));
	hc_test::Json all = f.pages();
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
}

TEST_CASE("Pages.WithoutPagesTheListsAreEmpty") {
	PagesFixture f;
	auto r = f.get("/api/pages", f.cookie);
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["loaded"].boolean);
	CHECK(r.json["pages"].items.empty());
	// An image of 1.0 with `ui: {}` loads, keeps its model, and has no pages: the UI falls back to the
	// generated overview.
	f.load(fixture("example.json"));
	r = f.get("/api/pages", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["total"].number == 0);
	CHECK(r.json["pages"].items.empty());
	CHECK(r.json["elements"].items.empty());
	CHECK(f.get("/api/model", f.cookie).json["functions"].items.size() == 9);
	// A swap to an image with pages and back: the lists follow the image.
	f.load(fixture("pages.json"));
	CHECK(f.get("/api/pages", f.cookie).json["pages"].items.size() == 2);
	f.load(fixture("minimal.json"));
	CHECK(f.get("/api/pages", f.cookie).json["pages"].items.empty());
}

TEST_CASE("Pages.LimitsAreRefusedBeyondTheirMaximum") {
	Plan plan;
	Report report;
	// At the limits: loads.
	CHECK_MESSAGE(parse(generated(16, 8, 4), plan, report), describe(report));
	CHECK_MESSAGE(parse(generated(16, 8, 1, 2, 1), plan, report), describe(report));
	// One beyond each.
	refused_at(generated(17, 1, 1), "ui.pages", "17 pages exceed the limit of 16");
	refused_at(generated(1, 129, 0), "ui.pages", "129 sections exceed the limit of 128");
	refused_at(generated(1, 1, 513), "ui.pages", "513 elements exceed the limit of 512");
	refused_at(generated(1, 1, 1, 257, 0), "ui.pages", "257 chart series exceed the limit of 256");
	refused_at(generated(1, 1, 1, 1, 129), "ui.pages", "129 reference lines exceed the limit of 128");
	// A page id is unique and shaped like the other ids; the member `ui` and `pages` have their types.
	refused_at(replace(generated(2, 1, 1), "\"id\": \"p1\"", "\"id\": \"p0\""), "ui.pages[1].id", "duplicate page id 'p0'");
	refused_at(replace(generated(1, 1, 1), "\"id\": \"p0\"", "\"id\": \"a/b\""), "ui.pages[0].id", "page id 'a/b' must have");
	refused_at(replace(generated(1, 1, 1), "\"ui\": {\"pages\": [", "\"ui\": {\"pages\": 3, \"x\": ["), "ui.pages",
		"'pages' must be an array");
}

#if defined(__GLIBC__)
TEST_CASE("Pages.MemoryOfTheKeptPageTable") {
	// What the UI model keeps for the pages (spec/runtime-image.md 5): the tables and the labels'
	// JSON text, on the 64-bit host (the device's pointers are half as wide).
	struct Case {
		const char* name;
		std::string with;
		std::string without;
	} cases[] = {
		{"pages.json", fixture("pages.json"), replace(fixture("pages.json"), "\"ui\": {\"pages\"", "\"ui\": {\"none\"")},
		{"512 datapoint elements", generated(16, 8, 4), replace(generated(16, 8, 4), "\"ui\": {\"pages\"", "\"ui\": {\"none\"")},
		{"128 charts, 256 series, 128 lines", generated(16, 8, 1, 2, 1),
			replace(generated(16, 8, 1, 2, 1), "\"ui\": {\"pages\"", "\"ui\": {\"none\"")},
	};
	for (const Case& c : cases) {
		PagesFixture f;
		f.load(c.without);
		size_t without = f.service.view().bytes();
		f.load(c.with);
		size_t with = f.service.view().bytes();
		MESSAGE(c.name << ": the page table keeps " << (with - without) << " B (UI model " << with << " B, without pages "
			<< without << " B)");
		CHECK(with > without);
	}
}
#endif

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
	// The chart queries the raw ring (the archives of issue #56 come later): a bucket's last value
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
	REQUIRE(bucket.items.size() == 6);
	CHECK(bucket.items[5].number > 0);
	CHECK(bucket.items[5].number < 0.5); // on for about one sample in four
}
