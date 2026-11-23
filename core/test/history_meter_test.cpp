/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The meter flag of a history and what a history covers (homecontrol #115, haumacher/ehome#221;
// spec/runtime-image.md 3.3, format 1.3): `history.meter` marks a meter reading, which the generic
// view draws as consumption bars; the device validates the flag (only a number is a meter reading),
// carries it and gives it back with the coverage, the longest retention of the ring and the archives.
// An image of format 1.2 and before has the byte 0 and loads unchanged.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/history.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/image/validator.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::image::ImageService;
using hc::image::MemoryImageSlot;
using hc_test::describe;

namespace {

struct Boot {
	hc_test::FakeBlobStore blobs;
	MemoryImageSlot slot;
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter knx{controller.loop(), p.clock, model, tunnel};
	ImageService service{controller, model, blobs, slot, knx, &tunnel};

	Boot() {
		controller.start();
		knx.start();
		service.start();
	}

	uint16_t id(const char* name) const {
		const hc::Datapoint* d = model.find(name);
		REQUIRE_MESSAGE(d != nullptr, "no datapoint " << name);
		return d->id();
	}
};

std::vector<uint8_t> fixture_bytes(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return std::vector<uint8_t>(text.begin(), text.end());
}

/// The corpus case: its `.bin` (E-Home's compiler writes the same bytes), else the source compiled.
std::vector<uint8_t> meter_case() {
	std::string text;
	if (hc_test::read_text(HC_SPEC_DIR "/fixtures/images/cases/accept-history-meter.bin", text)) {
		return std::vector<uint8_t>(text.begin(), text.end());
	}
	REQUIRE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/cases/accept-history-meter.json", text));
	return hc_test::compiled(text);
}

hc::image::bin::HistoryRec& history_record(hc_test::CompiledImage& image, size_t i) {
	const auto& r = image.view.at<hc::image::bin::HistoryRec>(hc::image::bin::kHistory, i);
	return *reinterpret_cast<hc::image::bin::HistoryRec*>(
		image.bytes.data() + (reinterpret_cast<const uint8_t*>(&r) - image.bytes.data()));
}

void fix_crc(std::vector<uint8_t>& b) {
	uint32_t crc = hc::image::bin::image_crc(b.data(), b.size());
	for (int i = 0; i < 4; ++i) {
		b[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}
}

} // namespace

TEST_CASE("HistoryMeter.TheCorpusCaseCarriesTheMeterFlagAndTheCoverage") {
	std::vector<uint8_t> bytes = meter_case();
	REQUIRE(bytes.size() > 32);
	CHECK(bytes[6] == 3); // format 1.3: the oldest that has the meter flag
	Boot boot;
	REQUIRE_MESSAGE(hc_test::deploy_bytes(boot.service, bytes), describe(boot.service.report()));
	const hc::History& history = boot.service.history();
	struct Expected {
		const char* name;
		uint32_t coverage;
		bool meter;
	};
	const Expected expected[] = {
		{"temperature", 30 * 86400, false}, // the longest archive (the ring keeps 24 h)
		{"energy", 365 * 86400, true},
		{"gas", 3650u * 86400u, true},
		{"counter", 86400, true}, // a ring alone
		{"light", 86400, false},  // meter false written out
		{"humidity", 21600, false}, // no member: as before 1.10
	};
	for (const Expected& e : expected) {
		CAPTURE(e.name);
		uint16_t id = boot.id(e.name);
		CHECK(history.has_history(id));
		CHECK(history.coverage_s(id) == e.coverage);
		CHECK(history.meter(id) == e.meter);
	}
}

TEST_CASE("HistoryMeter.AnImageOfAnOlderFormatLoadsUnchangedWithoutMeters") {
	// Format 1.2 (the initial values) and format 1.0 (the archives of #56): the byte was reserved and 0.
	for (const char* name : {"cases/accept-initial-values.bin", "cases/accept-history-archives.bin"}) {
		CAPTURE(name);
		std::vector<uint8_t> bytes = fixture_bytes(name);
		CHECK(bytes[6] < 3);
		Boot boot;
		REQUIRE_MESSAGE(hc_test::deploy_bytes(boot.service, bytes), describe(boot.service.report()));
		for (uint16_t id = 0; id < boot.model.size(); ++id) {
			CHECK_FALSE(boot.service.history().meter(id));
		}
	}
	std::vector<uint8_t> archives = fixture_bytes("cases/accept-history-archives.bin");
	Boot boot;
	REQUIRE(hc_test::deploy_bytes(boot.service, archives));
	// The temperature: a ring of 2 days, archives of 31 days and of 2 years; the heating a ring of a day and an archive of a year.
	CHECK(boot.service.history().coverage_s(boot.id("t")) == 63072000);
	CHECK(boot.service.history().coverage_s(boot.id("heating")) == 31536000);
	// A datapoint without a history covers nothing.
	CHECK(boot.service.history().coverage_s(999) == 0);
	CHECK_FALSE(boot.service.history().meter(999));
}

TEST_CASE("HistoryMeter.TheDeviceRefusesAMalformedFlag") {
	const char* source = R"({"image": {"schemaVersion": "1.10", "projectId": "refuse", "revision": 1},
"datapoints": [
 {"id": "energy", "type": "unsigned", "virtual": {}, "history": {"retention": 86400, "spacing": 300, "meter": true}},
 {"id": "heating", "type": "boolean", "virtual": {}, "history": {"retention": 86400, "spacing": 300}}]})";
	struct Mutation {
		const char* what;
		size_t record;
		uint8_t flags;
		const char* path;
		const char* reason;
	};
	const Mutation mutations[] = {
		{"an unknown bit", 0, 0x03, "history[0].flags", "malformed: unknown bits 0x2 in flags"},
		{"a meter of a boolean", 1, 0x01, "history[1].flags",
			"malformed: a meter of a boolean datapoint: only unsigned, signed and real"},
	};
	for (const Mutation& m : mutations) {
		CAPTURE(m.what);
		hc_test::CompiledImage image;
		REQUIRE(image.compile(source));
		CHECK(image.bytes[6] == 3);
		history_record(image, m.record).flags = m.flags;
		fix_crc(image.bytes);
		hc::image::Report report;
		CHECK_FALSE(hc::image::check_structure(image.bytes.data(), report));
		bool found = false;
		for (size_t i = 0; i < report.count; ++i) {
			found = found || (std::string(report.refusals[i].path) == m.path && std::strstr(report.refusals[i].reason, m.reason));
		}
		CHECK_MESSAGE(found, describe(report));
	}
	// The compiled source itself loads: the meter on the unsigned datapoint.
	hc_test::CompiledImage image;
	REQUIRE(image.compile(source));
	CHECK(history_record(image, 0).flags == 0x01);
	CHECK(history_record(image, 1).flags == 0x00);
	Boot boot;
	REQUIRE_MESSAGE(hc_test::deploy_bytes(boot.service, image.bytes), describe(boot.service.report()));
	CHECK(boot.service.history().meter(boot.id("energy")));
	CHECK_FALSE(boot.service.history().meter(boot.id("heating")));
}
