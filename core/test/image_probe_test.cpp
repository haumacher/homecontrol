/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the binary image's validation (issue #87, spec/runtime-image.md 10, I7): every image the
// device accepts must be safe to use in place. Single-bit damages of real images, with the CRC
// written anew so the structure is what is checked, are refused with a reason or accepted and then
// built and applied without a fault (run under ASan: no read outside the image, no crash).

#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/image/image_service.h"
#include "hc/image/loader.h"
#include "hc/image/memory_image_slot.h"
#include "hc/image/validator.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

struct Device {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::MemoryImageSlot slot{131072};
	hc::image::ImageService service{controller, model, blobs, slot, adapter, &tunnel};

	Device() {
		controller.start();
		adapter.start();
		service.start();
	}
};

void fix_crc(std::vector<uint8_t>& b) {
	const auto& h = *reinterpret_cast<const hc::image::bin::Header*>(b.data());
	size_t end = (h.flags & 1) != 0 ? h.length - 64 : h.length;
	uint32_t crc = hc::image::bin::image_crc(b.data(), end);
	for (int i = 0; i < 4; ++i) {
		b[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}
}

} // namespace

TEST_CASE("ImageProbe.BitFlipsAreRefusedOrSafe") {
	struct Source {
		const char* name;
		std::string json;
	} sources[] = {{"example.json", fixture("example.json")}, {"heating-timeline.json", fixture("heating-timeline.json")},
		{"pages.json", fixture("pages.json")}, {"zigbee-bindings.json", fixture("zigbee-bindings.json")}};
	Device device; // one device takes every accepted mutation in turn: swaps from anything to anything
	for (const Source& source : sources) {
		CAPTURE(source.name);
		std::vector<uint8_t> image = hc_test::compiled(source.json);
		size_t accepted = 0, refused = 0, applied = 0;
		// Every byte of the header, the section table and the first records; every 5th beyond.
		for (size_t at = 16; at < image.size(); at += at < 1024 ? 1 : 5) {
			for (uint8_t bit : {0x01, 0x80}) {
				std::vector<uint8_t> bytes = image;
				bytes[at] ^= bit;
				fix_crc(bytes);
				hc::image::Report report;
				bool ok = hc::image::check_header(bytes.data(), bytes.size(), bytes.size(), report) &&
					hc::image::check_crc(bytes.data(), report) && hc::image::check_structure(bytes.data(), report);
				if (!ok) {
					CHECK(report.count > 0); // a refusal speaks
					++refused;
					continue;
				}
				++accepted;
				// Accepted: the plan builds from it in place (or refuses by a catalogue check of 10.8).
				hc::image::bin::View view;
				view.open(bytes.data());
				hc::image::Plan plan;
				hc::image::Report built;
				hc_test::FakeClock clock;
				hc::image::Loader loader(clock, hc::HistoryLimits().cap_bytes);
				loader.build(view, plan, built);
				// Every 7th accepted one through the whole device: receive, apply, run a little.
				if (accepted % 7 == 0) {
					device.service.load(bytes.data(), bytes.size(), hc::image::Source::api);
					device.controller.run_once();
					++applied;
				}
			}
		}
		MESSAGE(source.name << ": " << image.size() << " bytes, " << refused << " damages refused, " << accepted
			<< " accepted (" << applied << " applied)");
		CHECK(refused > 0);
		// The undamaged image still loads after all of that.
		CHECK_MESSAGE(device.service.load(image.data(), image.size(), hc::image::Source::api),
			hc_test::describe(device.service.report()));
	}
}
