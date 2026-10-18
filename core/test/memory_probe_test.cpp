/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the memory work of issue #44 composed with the image deployment: the arena's blocks no
// longer waste a 4 KB tail, which the load's summary line shows as used-of-allocated figures close
// to each other, and a second deployment frees what the first allocated for its refusals.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "web_test_support.h"

using hc::HttpMethod;

namespace {

/// The two "X of Y bytes used" pairs of the load's summary line, or false.
bool parse_usage(const std::string& line, unsigned& used, unsigned& allocated) {
	const char* at = std::strstr(line.c_str(), "load tables ");
	return at != nullptr && std::sscanf(at, "load tables %u of %u bytes used", &used, &allocated) == 2;
}

} // namespace

TEST_CASE("Memory probe: the load's arena wastes less than a block, and refusal storage goes with the next load") {
	hc_test::WebFixture f;
	f.controller.start();
	std::string cookie = f.onboard();
	hc_test::FakeUdpSocket socket{f.fake.clock};
	hc::knx::KnxTunnel tunnel{f.controller.loop(), f.fake.clock, f.controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{f.controller.loop(), f.fake.clock, f.model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{f.controller, f.model, blobs, adapter, &tunnel};
	service.start();
	f.api.set_image(&service);
	hc_test::ScopedCapture capture;

	std::ifstream in(HC_SPEC_DIR "/fixtures/images/example.json");
	REQUIRE(in.good());
	std::stringstream buffer;
	buffer << in.rdbuf();
	std::string image = buffer.str();
	auto r = f.request(HttpMethod::put, "/api/image", image, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	bool found = false;
	for (const std::string& line : capture.sink.lines) {
		unsigned used = 0;
		unsigned allocated = 0;
		if (parse_usage(line, used, allocated)) {
			found = true;
			CHECK(used <= allocated);
			CHECK(allocated - used < 1024); // at most one block's tail
			CHECK(used > 0);
		}
	}
	CHECK(found);

	// A refused image fills the report (listed in the 422 answer); the running image stays, and the
	// next good load clears the report (GET /api/image is the stored image itself, so the status says).
	std::string broken = image;
	size_t at = broken.find("\"type\": \"boolean\"");
	REQUIRE(at != std::string::npos);
	broken.replace(at, std::strlen("\"type\": \"boolean\""), "\"type\": \"nonsense\"");
	r = f.request(HttpMethod::put, "/api/image", broken, cookie);
	CHECK(r.status == 422);
	CHECK(r.json["refusals"].items.size() >= 1);
	CHECK(r.json["loaded"].boolean); // the previous image still runs
	auto status = f.get("/api/status", cookie);
	REQUIRE(status.status == 200);
	CHECK(status.json["image"]["loaded"].boolean);
	CHECK_FALSE(status.json["image"].has("refused"));
	r = f.request(HttpMethod::put, "/api/image", image, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	status = f.get("/api/status", cookie);
	CHECK(status.json["image"]["loaded"].boolean);
	CHECK_FALSE(status.json["image"].has("refused"));
}
