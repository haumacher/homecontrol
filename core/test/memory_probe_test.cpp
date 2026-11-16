/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the memory work of issue #44 composed with the image deployment (issue #87: the binary
// image in the slot): the arena's blocks waste less than a block, the load's summary line reports
// what the plan's tables took, and a second deployment frees what the first allocated for its
// refusals.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/image/loader.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;

namespace {

/// The table bytes of the load's summary line ("..., tables N B"), or false.
bool parse_tables(const std::string& line, unsigned& bytes) {
	if (line.find(" loaded from ") == std::string::npos) {
		return false;
	}
	const char* at = std::strstr(line.c_str(), ", tables ");
	return at != nullptr && std::sscanf(at, ", tables %u B", &bytes) == 1;
}

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

std::string as_text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

} // namespace

TEST_CASE("Memory probe: the load's arena wastes less than a block, and refusal storage goes with the next load") {
	hc_test::WebFixture f;
	f.controller.start();
	std::string cookie = f.onboard();
	hc_test::FakeUdpSocket socket{f.fake.clock};
	hc::knx::KnxTunnel tunnel{f.controller.loop(), f.fake.clock, f.controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{f.controller.loop(), f.fake.clock, f.model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot;
	hc::image::ImageService service{f.controller, f.model, blobs, slot, adapter, &tunnel};
	service.start();
	f.api.set_image(&service);
	hc_test::ScopedCapture capture;

	std::string source = fixture("example.json");
	std::string image = as_text(hc_test::compiled(source));

	// The plan of the same image, built as the device builds it: its arena's blocks are used up to
	// less than a block's tail.
	hc_test::CompiledImage compiled;
	REQUIRE(compiled.compile(source));
	hc::image::Plan plan;
	hc::image::Report report;
	hc::image::Loader loader(f.fake.clock, hc::HistoryLimits().cap_bytes);
	REQUIRE_MESSAGE(loader.build(compiled.view, plan, report), hc_test::describe(report));
	CHECK(plan.arena.used() > 0);
	CHECK(plan.arena.used() <= plan.arena.bytes());
	CHECK(plan.arena.bytes() - plan.arena.used() < hc::image::Arena::kBlockSize); // at most one block's tail

	auto r = f.request(HttpMethod::put, "/api/image", image, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	bool found = false;
	for (const std::string& line : capture.sink.lines) {
		unsigned bytes = 0;
		if (parse_tables(line, bytes)) {
			found = true;
			CHECK(bytes == plan.arena.bytes()); // the summary line says what the tables took
		}
	}
	CHECK(found);

	// An image the device refuses by its own catalogue (DPT 250.001 it does not know, spec/runtime-image.md
	// 10.8) fills the report (listed in the 422 answer). It was written into the slot, so no image
	// runs then (I9) and the status says why; the next good load clears the report.
	std::string broken = source;
	size_t at = broken.find("\"dpt\": \"9.001\"");
	REQUIRE(at != std::string::npos);
	broken.replace(at, std::strlen("\"dpt\": \"9.001\""), "\"dpt\": \"250.001\"");
	r = f.request(HttpMethod::put, "/api/image", as_text(hc_test::compiled(broken)), cookie);
	CHECK(r.status == 422);
	CHECK(r.json["refusals"].items.size() >= 1);
	CHECK_FALSE(r.json["loaded"].boolean);
	auto status = f.get("/api/status", cookie);
	REQUIRE(status.status == 200);
	CHECK_FALSE(status.json["image"]["loaded"].boolean);
	CHECK(status.json["image"].has("refused"));
	r = f.request(HttpMethod::put, "/api/image", image, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	status = f.get("/api/status", cookie);
	CHECK(status.json["image"]["loaded"].boolean);
	CHECK_FALSE(status.json["image"].has("refused"));
	CHECK(service.report().count == 0);
}
