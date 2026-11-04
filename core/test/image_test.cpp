/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The runtime image (issue #8, spec/runtime-image.md): the loader against the fixtures in
// spec/fixtures/images, the chunked store, the service's atomic apply and swap, the LED inputs and
// the /api/image endpoints.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_image_staging.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/image_body.h"
#include "hc/image/image_service.h"
#include "hc/image/image_store.h"
#include "hc/image/loader.h"
#include "hc/image/memory_blob_store.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/value_store.h"
#include "hc/web_api.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::DatapointFlags;
using hc::ValueType;
using hc::image::ImageService;
using hc::image::ImageStore;
using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;
using hc::image::Source;
using hc_test::Bytes;
using hc_test::Json;

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

/// The test-system template with its "GA:<name>" placeholders replaced by 31/7/n, in order of first
/// appearance (addresses no real project here uses); `addresses` maps each name to its raw address.
std::string fill_template(std::map<std::string, uint16_t>* addresses = nullptr) {
	std::string text = fixture("test-system.template.json");
	std::map<std::string, uint16_t> names;
	std::string out;
	size_t pos = 0;
	for (;;) {
		size_t at = text.find("\"GA:", pos);
		while (at != std::string::npos && at > 0 && text[at - 1] == '\\') {
			at = text.find("\"GA:", at + 1); // the comment's \"GA:<name>\"
		}
		if (at == std::string::npos) {
			out += text.substr(pos);
			break;
		}
		size_t end = text.find('"', at + 1);
		std::string name = text.substr(at + 4, end - at - 4);
		if (names.count(name) == 0) {
			uint16_t n = static_cast<uint16_t>(names.size() + 1);
			names[name] = static_cast<uint16_t>((31 << 11) | (7 << 8) | n);
		}
		uint16_t raw = names[name];
		out += text.substr(pos, at - pos);
		out += "\"31/7/" + std::to_string(raw & 0xFF) + "\"";
		pos = end + 1;
	}
	if (addresses != nullptr) {
		*addresses = names;
	}
	return out;
}

/// Replaces the first occurrence of `from` in `text` by `to`.
std::string replace(std::string text, const std::string& from, const std::string& to) {
	size_t at = text.find(from);
	REQUIRE_MESSAGE(at != std::string::npos, "no '" << from << "'");
	text.replace(at, from.size(), to);
	return text;
}

std::string hex2(uint16_t raw) {
	char buffer[8];
	std::snprintf(buffer, sizeof(buffer), "%02X %02X", raw >> 8, raw & 0xFF);
	return buffer;
}

/// A controller with the image service, a KNX tunnel and adapter on the controller's loop, and an
/// in-memory blob store.
struct ServiceFixture {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	ImageService service{controller, model, blobs, adapter, &tunnel};
	size_t handled = 0;
	uint8_t gateway_sequence = 0;
	std::vector<std::string> telegrams; ///< "dest len tpdu" of each group telegram the device sent.

	explicit ServiceFixture(const std::map<std::string, std::vector<uint8_t>>* stored = nullptr) {
		if (stored != nullptr) {
			blobs.blobs = *stored;
		}
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		service.start();
	}

	bool load(const std::string& text, Source source = Source::api) {
		return service.load(text.data(), text.size(), source, true);
	}

	/// The tunnel to the test gateway is up on channel 7.
	void connect() {
		std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse("192.0.2.1");
		REQUIRE(gateway);
		tunnel.set_gateway(&*gateway, false);
		tunnel.start();
		run_for(0);
		socket.inject(hc_test::connect_response(7));
		REQUIRE(tunnel.is_up());
		pump();
	}

	/// Acknowledges and confirms every TUNNELING_REQUEST sent, recording its telegram.
	void pump() {
		for (int round = 0; round < 100; ++round) {
			controller.run_once();
			if (handled == socket.sent.size()) {
				return;
			}
			while (handled < socket.sent.size()) {
				Bytes request = socket.sent[handled++].data;
				if (request.size() >= 8 && request[2] == 0x02 && request[3] == 0x07) {
					socket.inject(hc_test::connectionstate_response(7)); // the heartbeat
					continue;
				}
				if (request.size() < 10 || request[2] != 0x04 || request[3] != 0x20) {
					continue;
				}
				telegrams.push_back(hc_test::hex(Bytes(request.begin() + 16, request.end())));
				socket.inject(hc_test::tunneling_ack(7, request[8]));
				Bytes cemi(request.begin() + 10, request.end());
				cemi[0] = 0x2E;
				socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
			}
		}
	}

	/// A GroupValue_Write from 1.1.10 to `destination`.
	void receive(uint16_t destination, const char* tpdu) {
		Bytes cemi = hc_test::bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = hc_test::bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
		pump();
	}

	/// Runs the loop, jumping from deadline to deadline, for `ms`, answering the tunnel's requests.
	void run_for(uint64_t ms) {
		uint64_t until = p.clock.now_ms + ms;
		for (;;) {
			pump();
			uint64_t deadline = controller.next_deadline();
			if (deadline > until) {
				p.clock.now_ms = until;
				pump();
				return;
			}
			if (deadline > p.clock.now_ms) {
				p.clock.now_ms = deadline;
			}
		}
	}

	bool sent(const std::string& telegram) const {
		return std::find(telegrams.begin(), telegrams.end(), telegram) != telegrams.end();
	}

	const hc::Datapoint& dp(const char* name) const {
		const hc::Datapoint* d = model.find(name);
		REQUIRE_MESSAGE(d != nullptr, "no datapoint " << name);
		return *d;
	}
};

} // namespace

// ---- Loader -------------------------------------------------------------------------------------

TEST_CASE("Image.Corpus") {
	std::vector<std::string> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(HC_SPEC_DIR "/fixtures/images/cases", error)) {
		if (entry.path().extension() == ".json") {
			files.push_back(entry.path().filename().string());
		}
	}
	std::sort(files.begin(), files.end());
	REQUIRE(files.size() >= 10);
	for (const std::string& file : files) {
		CAPTURE(file);
		std::string text = fixture("cases/" + file);
		Json doc;
		std::string parse_error;
		REQUIRE(hc_test::parse_json(text, doc, parse_error));
		const Json& expect = doc["expect"];
		REQUIRE(expect.kind == Json::Kind::object);
		Plan plan;
		Report report;
		bool ok = parse(text, plan, report);
		std::string got = describe(report);
		CAPTURE(got);
		CHECK(ok == expect["valid"].boolean);
		if (ok) {
			continue;
		}
		CHECK(std::string(hc::image::report_kind_name(report.kind)) == expect["kind"].text);
		const Json& refusals = expect["refusals"];
		CHECK(report.total == refusals.items.size());
		for (const Json& refusal : refusals.items) {
			bool found = false;
			for (size_t i = 0; i < report.count && !found; ++i) {
				found = refusal["path"].text == report.refusals[i].path &&
					std::strstr(report.refusals[i].reason, refusal["reason"].text.c_str()) != nullptr;
			}
			CHECK_MESSAGE(found, "expected " << refusal["path"].text << ": ..." << refusal["reason"].text << "...");
		}
	}
}

TEST_CASE("Image.NewerMinorNamedInStrictRefusals") {
	// From 1.2 on a strict refusal of an image of a newer minor version says the firmware is older
	// (spec/runtime-image.md I4); an image of the version this firmware reads, or an older one, does not.
	const char* schedule = R"(, "schedules": [{"id": "s", "label": "S", "kind": "weekly",
		"time": {"hour": 6, "minute": 0, "second": 0}, "jitter": 1}]})";
	char newer[16];
	std::snprintf(newer, sizeof(newer), "%u.%u", static_cast<unsigned>(hc::image::kSchemaMajor),
		static_cast<unsigned>(hc::image::kSchemaMinor + 1));
	for (const char* version : {hc::image::schema_version_text(), "1.0", static_cast<const char*>(newer)}) {
		CAPTURE(version);
		std::string text = std::string(R"({"image": {"schemaVersion": ")") + version +
			R"(", "projectId": "p", "revision": 1}, "datapoints": [])" + schedule;
		Plan plan;
		Report report;
		REQUIRE_FALSE(parse(text, plan, report));
		REQUIRE(report.count == 1);
		std::string reason = report.refusals[0].reason;
		if (version == newer) {
			CHECK(reason == std::string("malformed: unknown member 'jitter' (the image's schema ") + newer +
				" is newer than this firmware reads, up to " + hc::image::schema_version_text() + ": update the firmware)");
		} else {
			CHECK(reason == "malformed: unknown member 'jitter'");
		}
	}
	// A button's presses are strict too (1.4, haumacher/ehome #70): a kind of a newer minor names it.
	for (const char* version : {hc::image::schema_version_text(), static_cast<const char*>(newer)}) {
		CAPTURE(version);
		std::string text = std::string(R"({"image": {"schemaVersion": ")") + version +
			R"(", "projectId": "p", "revision": 1}, "datapoints": [{"id": "r", "type": "enumeration",
			"zigbee": {"ieee": "000b57fffe112233", "endpoint": 1, "cluster": 6, "client": true}}],
			"functions": [{"id": "f", "label": "F", "kind": "button", "presses": {"on": [1], "doubleClick": [2]},
			"datapoints": {"press": "r"}}]})";
		Plan plan;
		Report report;
		REQUIRE_FALSE(parse(text, plan, report));
		REQUIRE(report.count == 1);
		CHECK(std::string(report.refusals[0].path) == "functions[0].presses");
		std::string reason = report.refusals[0].reason;
		if (version == newer) {
			CHECK(reason == std::string("malformed: unknown member 'doubleClick' (the image's schema ") + newer +
				" is newer than this firmware reads, up to " + hc::image::schema_version_text() + ": update the firmware)");
		} else {
			CHECK(reason == "malformed: unknown member 'doubleClick'");
		}
	}
	// A long rule id and member: the reason is cut, never the hint.
	std::string id(32, 'r'); // the longest rule id
	std::string member(39, 'm');
	std::string text = std::string(R"({"image": {"schemaVersion": ")") + newer +
		R"(", "projectId": "p", "revision": 1}, "datapoints": [], "rules": [{"id": ")" + id + R"(", "name": "R", ")" + member +
		R"(": 1, "reactions": [{"triggers": [{"kind": "start"}], "actions": []}]}]})";
	Plan plan;
	Report report;
	REQUIRE_FALSE(parse(text, plan, report));
	REQUIRE(report.count == 1);
	std::string reason = report.refusals[0].reason;
	CAPTURE(reason);
	CHECK(reason.rfind("rule '" + id, 0) == 0);
	CHECK(reason.size() <= Report::kMaxReason);
	CHECK(reason.find(std::string("up to ") + hc::image::schema_version_text() + ": update the firmware)") != std::string::npos);
}

TEST_CASE("Image.ExampleAndMinimalLoad") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(fixture("example.json"), plan, report), describe(report));
	CHECK(std::string(plan.info.project_id) == "example-house");
	CHECK(plan.info.revision == 7);
	CHECK(plan.datapoint_count == 21); // 15, the time master's knx.time, knx.date, four rule parameters (spec/rules.md 8.6)
	CHECK(plan.info.knx_datapoints == 13);
	CHECK(plan.info.rules == 2);
	CHECK(plan.info.schedules == 2);
	CHECK(plan.info.functions == 9);
	CHECK(plan.info.locations == 4);
	CHECK(plan.info.history_rings == 1);
	CHECK(plan.info.time_master);
	CHECK(plan.has_tz);
	CHECK(plan.history_budget == 4096);
	MESSAGE("example.json: " << fixture("example.json").size() << " bytes, Plan " << sizeof(Plan) << ", Report " << sizeof(Report) << ", load tables " << plan.arena.bytes()
		<< " bytes");

	// Issue #44: the arena's blocks follow the content (1 KB blocks, a block of its own for a large
	// table), not 4 KB per arena whatever it holds.
	CHECK(plan.arena.used() <= plan.arena.bytes());
	CHECK(plan.arena.bytes() - plan.arena.used() < 2 * hc::image::Arena::kBlockSize);

	Plan minimal;
	REQUIRE_MESSAGE(parse(fixture("minimal.json"), minimal, report), describe(report));
	CHECK(minimal.datapoint_count == 0);
	CHECK(minimal.arena.bytes() < 2048);
	CHECK_FALSE(minimal.has_rules);
	CHECK_FALSE(minimal.has_time_master);
}

TEST_CASE("Image.ArenaSmallPlanAndLargeTables") {
	// A small plan allocates under 2 KB.
	Plan plan;
	Report report;
	std::string small = R"({"image": {"schemaVersion": "1.0", "projectId": "p", "revision": 1}, )"
		R"("datapoints": [{"id": "a", "type": "boolean"}, {"id": "b", "type": "real"}]})";
	REQUIRE_MESSAGE(parse(small, plan, report), describe(report));
	CHECK(plan.datapoint_count == 2);
	CHECK(plan.arena.bytes() < 2048);
	MESSAGE("small plan: " << plan.arena.used() << " of " << plan.arena.bytes() << " bytes used");

	// A large table gets a block of its own; the small ones keep filling the shared block.
	hc::image::Arena arena;
	CHECK(arena.allocate(100) != nullptr);
	size_t one = arena.bytes();
	CHECK(arena.allocate(5000) != nullptr);
	CHECK(arena.allocate(100) != nullptr); // still in the first shared block
	CHECK(arena.bytes() - one < 5000 + 64);
	CHECK(arena.used() == 104 + 5000 + 104);
	arena.release();
	CHECK(arena.bytes() == 0);
	CHECK(arena.used() == 0);
}

TEST_CASE("Image.ReportHoldsOneRefusalInlineAndAllocatesTheRestOnDemand") {
	// Issue #44: the service keeps its report for GET /api/image; without refusals it costs one.
	static_assert(sizeof(Report) < 2 * sizeof(Report::Refusal), "the room for twelve refusals is not inline");
	hc_test::AllocationScope counting;
	{
		Report report;
		report.add(Report::Kind::invalid, "a", "first %d", 1);
		CHECK(counting.count() == 0); // the first is inline
		for (int i = 2; i <= 14; ++i) {
			report.add(Report::Kind::apply, "b", "refusal %d", i);
		}
		CHECK(report.kind == Report::Kind::invalid);
		CHECK(report.count == Report::kMaxRefusals);
		CHECK(report.total == 14);
		CHECK(std::string(report.refusals[0].reason) == "first 1");
		CHECK(std::string(report.refusals[11].reason) == "refusal 12");
		CHECK(counting.live() == Report::kMaxRefusals * sizeof(Report::Refusal));
		report.clear(); // the next load: the room goes
		CHECK(counting.live() == 0);
		CHECK(report.count == 0);
		report.add(Report::Kind::syntax, "", "again");
		CHECK(std::string(report.refusals[0].reason) == "again");
		CHECK(counting.live() == 0);
	}
	CHECK(counting.live() == 0);
}

TEST_CASE("Image.TestSystemTemplate") {
	// Unfilled, every placeholder is refused with its path.
	Plan raw;
	Report report;
	CHECK_FALSE(parse(fixture("test-system.template.json"), raw, report));
	CHECK(report.kind == Report::Kind::invalid);
	CHECK(std::string(report.refusals[0].path) == "knx.timeMaster.time");
	CHECK(std::string(report.refusals[1].path) == "datapoints[0].knx.listen[0]");

	std::map<std::string, uint16_t> addresses;
	std::string filled = fill_template(&addresses);
	CHECK(addresses.size() == 16); // six keys, four channels with state, the temperature, the time
	Plan plan;
	REQUIRE_MESSAGE(parse(filled, plan, report), describe(report));
	CHECK(plan.datapoint_count == 19); // 15, knx.time and three rules' enable switches
	CHECK(plan.info.functions == 11);
	CHECK(plan.info.rules == 3);
	CHECK(plan.info.schedules == 1);
	CHECK(plan.info.history_rings == 1);
	MESSAGE("test-system template: " << filled.size() << " bytes, load tables " << plan.arena.bytes() << " bytes");
}

TEST_CASE("Image.SyntaxAndSizeRefusals") {
	Plan plan;
	Report report;
	CHECK_FALSE(parse("{\"image\": {\"schemaVersion\": \"1.0\",}", plan, report));
	CHECK(report.kind == Report::Kind::syntax);
	REQUIRE(report.count == 1);
	CHECK(std::strstr(report.refusals[0].reason, "at byte") != nullptr);

	Plan array;
	CHECK_FALSE(parse("[1, 2]", array, report));
	CHECK(report.kind == Report::Kind::invalid);

	std::string large(hc::image::kMaxImageBytes + 1, ' ');
	large[0] = '{';
	large[large.size() - 1] = '}';
	Plan big;
	CHECK_FALSE(parse(large, big, report));
	CHECK(report.kind == Report::Kind::too_large);

	// Deep but within kMaxNesting: refused for its content, not its depth.
	std::string deep = "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"d\", \"revision\": 1}, \"ui\": ";
	for (int i = 0; i < 40; ++i) {
		deep += "[";
	}
	for (int i = 0; i < 40; ++i) {
		deep += "]";
	}
	deep += "}";
	Plan nested;
	CHECK_FALSE(parse(deep, nested, report));
	CHECK(report.kind == Report::Kind::invalid);
	CHECK(std::string(report.refusals[0].path) == "ui");
}

TEST_CASE("Image.JsonDepthLimits") {
	std::string text;
	for (int i = 0; i < 48; ++i) {
		text += "[";
	}
	for (int i = 0; i < 48; ++i) {
		text += "]";
	}
	hc::JsonSpan span;
	const char* error = nullptr;
	CHECK_FALSE(hc::json_parse(text.data(), text.size(), span, &error));
	CHECK(hc::json_parse(text.data(), text.size(), span, &error, hc::image::kMaxNesting));
	std::string deeper = "[" + text + "]";
	size_t offset = 0;
	CHECK_FALSE(hc::json_parse(deeper.data(), deeper.size(), span, &error, hc::image::kMaxNesting, &offset));
	CHECK(offset == 48);
	// The cap holds whatever the caller asks.
	std::string very(70, '[');
	very += std::string(70, ']');
	CHECK_FALSE(hc::json_parse(very.data(), very.size(), span, &error, 1000));
}

// ---- Store --------------------------------------------------------------------------------------

TEST_CASE("Image.StoreReadsIntoABodyInPieces") {
	// homecontrol #71: the stored image is read into an ImageBody (the stored staging, or the heap) in
	// pieces of kReadPiece, the CRC over the pieces; the stored format is the one every firmware wrote.
	hc_test::FakeBlobStore blobs;
	ImageStore store(blobs);
	std::string text(40001, 'x');
	for (size_t i = 0; i < text.size(); ++i) {
		text[i] = static_cast<char>('a' + i % 26);
	}
	const char* error = nullptr;
	REQUIRE(store.write(text.data(), text.size(), &error));
	size_t bytes = 0;
	REQUIRE(store.stored(bytes));

	hc_test::FakeImageStaging staging;
	staging.content.reserve(bytes + 1); // the fake's buffer is flash on the device: no growth in the scope
	hc::image::ImageBody body;
	REQUIRE(body.begin(bytes, &staging));
	REQUIRE(body.staged());
	bool missing = false;
	{
		hc_test::AllocationScope scope;
		REQUIRE(store.read(body, missing, &error));
		CHECK(scope.largest() == ImageStore::kReadPiece); // the piece, no block of the image's size
	}
	CHECK(std::string(body.text(), body.size()) == text);
	CHECK(body.text()[body.size()] == '\0');
	// Two chunks of 16384 in 8 pieces each, the last 7233 bytes in 4, and the NUL of finish().
	CHECK(staging.appends == 8 + 8 + 4 + 1);
	body.release();

	// Into the heap (no staging, the POSIX build).
	REQUIRE(body.begin(bytes, nullptr));
	REQUIRE(store.read(body, missing, &error));
	CHECK(std::string(body.text(), body.size()) == text);
	body.release();

	// A body of another size (the image changed after stored()) is refused.
	REQUIRE(body.begin(bytes - 1, nullptr));
	CHECK_FALSE(store.read(body, missing, &error));
	CHECK(std::string(error).find("changed") != std::string::npos);
	body.release();

	// A staging that fails on the way is refused with the reason.
	staging.fail_after = 3;
	REQUIRE(body.begin(bytes, &staging));
	CHECK_FALSE(store.read(body, missing, &error));
	CHECK(std::string(error).find("staging") != std::string::npos);
	body.release();
	staging.fail_after = 0;

	// A damaged chunk fails the CRC; nothing is finished.
	blobs.blobs["image.a.1"][100] ^= 1;
	REQUIRE(body.begin(bytes, &staging));
	CHECK_FALSE(store.read(body, missing, &error));
	CHECK(std::string(error).find("checksum") != std::string::npos);
	CHECK_FALSE(staging.finished);
	body.release();

	// The format as an earlier firmware stored it, written by hand: manifest "HCIMG 1 <gen> <bytes>
	// <chunks> <crc32>" and chunks of 16384 bytes.
	hc_test::FakeBlobStore old;
	std::string image = "{\"image\": {\"schemaVersion\": \"1.0\"}}" + std::string(20000, ' ');
	old.blobs["image.b.0"] = std::vector<uint8_t>(image.begin(), image.begin() + 16384);
	old.blobs["image.b.1"] = std::vector<uint8_t>(image.begin() + 16384, image.end());
	char manifest[80];
	std::snprintf(manifest, sizeof(manifest), "HCIMG 1 b %u 2 %08x\n", static_cast<unsigned>(image.size()),
		static_cast<unsigned>(hc::persist::crc32(0, image.data(), image.size())));
	old.blobs["image.idx"] = std::vector<uint8_t>(manifest, manifest + std::strlen(manifest));
	ImageStore earlier(old);
	REQUIRE(earlier.stored(bytes));
	REQUIRE(body.begin(bytes, &staging));
	REQUIRE(earlier.read(body, missing, &error));
	CHECK(std::string(body.text(), body.size()) == image);
	body.release();

	// None stored.
	hc_test::FakeBlobStore empty;
	ImageStore none(empty);
	REQUIRE(body.begin(10, nullptr));
	CHECK_FALSE(none.read(body, missing, &error));
	CHECK(missing);
}


TEST_CASE("Image.StoreChunksGenerationsAndCrc") {
	hc_test::FakeBlobStore blobs;
	ImageStore store(blobs);
	size_t bytes = 0;
	CHECK_FALSE(store.stored(bytes));

	std::string text(40000, 'x');
	for (size_t i = 0; i < text.size(); ++i) {
		text[i] = static_cast<char>('a' + i % 26);
	}
	const char* error = nullptr;
	REQUIRE(store.write(text.data(), text.size(), &error));
	REQUIRE(store.stored(bytes));
	CHECK(bytes == 40000);
	CHECK(blobs.blobs.count("image.idx") == 1);
	CHECK(blobs.blobs.count("image.a.0") == 1);
	CHECK(blobs.blobs.count("image.a.2") == 1);
	CHECK(blobs.blobs.count("image.a.3") == 0);

	std::string back(bytes, '\0');
	size_t length = 0;
	bool missing = false;
	REQUIRE(store.read(&back[0], back.size(), length, missing, &error));
	CHECK(length == bytes);
	CHECK(back == text);

	// Pieces across chunk borders.
	char part[100];
	REQUIRE(store.read_part(store.token(), 16350, part, sizeof(part), length));
	CHECK(length == 100);
	CHECK(std::string(part, 100) == text.substr(16350, 100));
	CHECK_FALSE(store.read_part(store.token() + 1, 0, part, sizeof(part), length));

	// A second write goes to the other generation and removes the first.
	std::string second(20000, 'y');
	uint32_t first_token = store.token();
	REQUIRE(store.write(second.data(), second.size(), &error));
	CHECK(store.token() != first_token);
	CHECK(blobs.blobs.count("image.a.0") == 0);
	CHECK(blobs.blobs.count("image.b.0") == 1);
	CHECK(blobs.blobs.count("image.b.1") == 1);

	// A failing write keeps the stored image.
	blobs.fail_writes = true;
	CHECK_FALSE(store.write(text.data(), text.size(), &error));
	blobs.fail_writes = false;
	REQUIRE(store.stored(bytes));
	CHECK(bytes == 20000);
	std::string kept(bytes, '\0');
	REQUIRE(store.read(&kept[0], kept.size(), length, missing, &error));
	CHECK(kept == second);

	// Orphans of an interrupted write are collected.
	blobs.blobs["image.a.0"] = std::vector<uint8_t>(10, 0);
	blobs.blobs["image.a.7"] = std::vector<uint8_t>(10, 0);
	store.collect_garbage();
	CHECK(blobs.blobs.count("image.a.0") == 0);
	CHECK(blobs.blobs.count("image.a.7") == 0);
	CHECK(blobs.blobs.count("image.b.1") == 1);
	// More orphans than one pass collects (issue #44: a few names per pass, not a static table of all).
	for (int i = 0; i < 25; ++i) {
		blobs.blobs["image.a." + std::to_string(i)] = std::vector<uint8_t>(10, 0);
	}
	blobs.blobs["image.b.9"] = std::vector<uint8_t>(10, 0); // beyond the stored image's chunk count
	blobs.blobs["values.x"] = std::vector<uint8_t>(10, 0);  // not the image's
	store.collect_garbage();
	CHECK(blobs.blobs.count("image.a.24") == 0);
	CHECK(blobs.blobs.count("image.b.9") == 0);
	CHECK(blobs.blobs.count("image.b.0") == 1);
	CHECK(blobs.blobs.count("image.b.1") == 1);
	CHECK(blobs.blobs.count("values.x") == 1);
	blobs.blobs.erase("values.x");
	size_t image_blobs = 0;
	for (const auto& blob : blobs.blobs) {
		image_blobs += blob.first.rfind("image.", 0) == 0 ? 1 : 0;
	}
	CHECK(image_blobs == 3); // the manifest and the two chunks

	// A corrupted chunk fails the CRC.
	blobs.blobs["image.b.1"][5] ^= 1;
	CHECK_FALSE(store.read(&kept[0], kept.size(), length, missing, &error));
	CHECK(std::string(error).find("checksum") != std::string::npos);

	REQUIRE(store.remove(&error));
	CHECK_FALSE(store.stored(bytes));
	CHECK(blobs.blobs.empty());
}

TEST_CASE("Image.MemoryBlobStoreKeepsTheContract") {
	hc::image::MemoryBlobStore store(30000);
	std::string a(20000, 'a');
	std::string b(20481, 'b');
	hc::BlobChunk chunk{a.data(), a.size()};
	CHECK(store.write("one", &chunk, 1) == hc::BlobStatus::ok);
	CHECK(store.write(".hidden", &chunk, 1) == hc::BlobStatus::invalid_name);
	hc::BlobChunk large{b.data(), b.size()};
	CHECK(store.write("big", &large, 1) == hc::BlobStatus::too_large);
	CHECK(store.write("two", &chunk, 1) == hc::BlobStatus::no_space); // 40000 > 30000
	size_t size = 0;
	REQUIRE(store.size("one", size) == hc::BlobStatus::ok);
	CHECK(size == 20000);
	// Replacing counts the old content out.
	hc::BlobChunk pieces[2] = {{"he", 2}, {"llo", 3}};
	CHECK(store.write("one", pieces, 2) == hc::BlobStatus::ok);
	CHECK(store.used() == 5);
	char buffer[8];
	size_t length = 0;
	CHECK(store.read("one", 1, buffer, sizeof(buffer), length) == hc::BlobStatus::ok);
	CHECK(std::string(buffer, length) == "ello");
	CHECK(store.read("one", 9, buffer, sizeof(buffer), length) == hc::BlobStatus::ok);
	CHECK(length == 0);
	CHECK(store.read("none", 0, buffer, sizeof(buffer), length) == hc::BlobStatus::not_found);
	CHECK(store.write("two", &chunk, 1) == hc::BlobStatus::ok);
	CHECK(store.remove("one") == hc::BlobStatus::ok);
	CHECK(store.remove("one") == hc::BlobStatus::not_found);
	CHECK(store.used() == 20000);
	CHECK(store.size("two", size) == hc::BlobStatus::ok);

	// The image store on it: the image as chunks, read back whole.
	hc::image::MemoryBlobStore ram(96 * 1024);
	ImageStore images(ram);
	std::string text = fixture("example.json");
	const char* error = nullptr;
	REQUIRE(images.write(text.data(), text.size(), &error));
	std::string back(text.size(), '\0');
	bool missing = false;
	REQUIRE(images.read(&back[0], back.size(), length, missing, &error));
	CHECK(back == text);
}

// ---- Service ------------------------------------------------------------------------------------

TEST_CASE("Image.ServiceAppliesStoresAndReloads") {
	ServiceFixture f;
	CHECK_FALSE(f.service.loaded());
	std::string example = fixture("example.json");
	REQUIRE_MESSAGE(f.load(example), describe(f.service.report()));
	CHECK(f.service.loaded());
	CHECK(f.service.stored_ok());
	CHECK(f.model.size() == 21);
	CHECK(f.model.find("knx.time") != nullptr);
	CHECK(f.service.engine().rules().rule_count() == 2);
	CHECK(f.service.scheduler().size() == 2);
	CHECK(std::string(f.controller.time_zone()) == "CET-1CEST,M3.5.0,M10.5.0/3");
	CHECK(f.p.sink.contains("image 'example-house' revision 7 (Example house, schema 1.0) loaded from api"));
	size_t bytes = 0;
	REQUIRE(f.service.stored(bytes));
	CHECK(bytes == example.size());

	// A refused image leaves the loaded one running.
	CHECK_FALSE(f.load(replace(example, "\"revision\": 7", "\"revision\": \"8\"")));
	CHECK(f.service.loaded());
	CHECK(f.service.info().revision == 7);
	CHECK(f.service.report().kind == Report::Kind::invalid);
	CHECK(f.model.size() == 21);

	// After a restart, the stored image loads at boot and equals the pushed one.
	ServiceFixture h(&f.blobs.blobs);
	CHECK(h.service.loaded());
	CHECK(h.service.info().revision == 7);
	CHECK(h.service.info().bytes == example.size());
	CHECK(h.service.source() == Source::boot);
	CHECK(h.model.size() == 21);
	CHECK(h.service.stored_ok());
	CHECK(h.p.sink.contains("loaded from boot"));
}

TEST_CASE("Image.SwapKeepsValuesAndFollowsIds") {
	ServiceFixture f;
	f.connect();
	std::string example = fixture("example.json");
	REQUIRE_MESSAGE(f.load(example), describe(f.service.report()));
	f.pump();
	f.receive(0x0902, "00 81");    // 1/1/2 living.light.state on
	f.receive(0x0C02, "00 80 03 E8"); // 1/4/2 hall.lux 10 lx
	CHECK(f.dp("living.light.state").value() == hc::Value::of_bool(true));
	uint64_t updated = f.dp("living.light.state").updated_ms();

	// Revision 8 drops the first datapoint (every id shifts) and changes hall.lux's type.
	std::string next = replace(example, "\"revision\": 7", "\"revision\": 8");
	next = replace(next,
		"{\"id\": \"living.light\", \"label\": \"Living room light\", \"type\": \"boolean\", \"writable\": true, "
		"\"transmit\": true,\n\t\t \"knx\": {\"dpt\": \"1.001\", \"send\": \"1/1/1\"}},\n\t\t", "");
	next = replace(next,
		"{\"id\": \"living-light\", \"label\": \"Ceiling light\", \"room\": \"living\", \"kind\": \"light\",\n\t\t "
		"\"datapoints\": {\"command\": \"living.light\", \"state\": \"living.light.state\"}},", "");
	next = replace(next,
		"\"actions\": [{\"kind\": \"write\", \"datapoint\": \"living.light\", \"value\": {\"kind\": \"literal\", "
		"\"type\": \"boolean\", \"value\": true}}]",
		"\"actions\": []");
	REQUIRE_MESSAGE(f.load(next), describe(f.service.report()));
	f.p.clock.advance(10);
	CHECK(f.service.info().revision == 8);
	CHECK(f.model.find("living.light") == nullptr);
	CHECK(f.dp("living.light.state").id() == 0);
	CHECK(f.dp("living.light.state").value() == hc::Value::of_bool(true));
	CHECK(f.dp("living.light.state").updated_ms() == updated);
	CHECK(f.dp("hall.lux").value().as_real() == doctest::Approx(10.0));
	CHECK(f.p.sink.contains("rules started (image)"));

	// The presence rule runs on the new ids: presence in the dark switches the hall light (1/1/10).
	f.telegrams.clear();
	f.receive(0x0C01, "00 81"); // 1/4/1 hall.presence
	CHECK(f.sent("09 0A 01 00 81"));
	CHECK(f.dp("hall.light").value() == hc::Value::of_bool(true));
}

TEST_CASE("Image.TestSystemBehaviour") {
	ServiceFixture f;
	f.connect();
	std::map<std::string, uint16_t> ga;
	std::string image = fill_template(&ga);
	REQUIRE_MESSAGE(f.load(image), describe(f.service.report()));
	f.run_for(10000);
	// The states are read on start.
	CHECK(f.sent(hex2(ga["chA.state"]) + " 01 00 00"));
	CHECK(f.sent(hex2(ga["temperature"]) + " 01 00 00"));

	// Key 3 toggles channel C from its reported state.
	f.telegrams.clear();
	f.receive(ga["key3"], "00 81");
	CHECK(f.sent(hex2(ga["chC"]) + " 01 00 81"));
	f.receive(ga["chC.state"], "00 81");
	f.telegrams.clear();
	f.receive(ga["key3"], "00 80"); // whatever the key sends
	CHECK(f.sent(hex2(ga["chC"]) + " 01 00 80"));
	// The other keys are buttons only.
	f.telegrams.clear();
	f.receive(ga["key1"], "00 81");
	CHECK(f.telegrams.empty());
	CHECK(f.dp("key1").value() == hc::Value::of_bool(true));

	// Channel D switches off two minutes after it reports on.
	f.telegrams.clear();
	f.receive(ga["chD.state"], "00 81");
	f.run_for(119000);
	CHECK_FALSE(f.sent(hex2(ga["chD"]) + " 01 00 80"));
	f.run_for(1000);
	CHECK(f.sent(hex2(ga["chD"]) + " 01 00 80"));

	// The temperature persists and keeps a history.
	f.receive(ga["temperature"], "00 80 0C 1A"); // 21.0 °C
	CHECK(f.dp("temperature").value().as_real() == doctest::Approx(21.0));
	CHECK(f.dp("temperature").has_history());
	CHECK(f.dp("temperature").persists());
}

TEST_CASE("Image.UnloadWipes") {
	ServiceFixture f;
	REQUIRE(f.load(fixture("example.json")));
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	CHECK_FALSE(f.service.loaded());
	CHECK(f.model.size() == 0);
	CHECK(f.service.engine().rules().rule_count() == 0);
	CHECK(f.service.scheduler().size() == 0);
	size_t bytes = 0;
	CHECK_FALSE(f.service.stored(bytes));
	CHECK(f.blobs.blobs.count("image.idx") == 0);
	// Loads again after the wipe.
	REQUIRE(f.load(fixture("minimal.json")));
	CHECK(f.service.loaded());
	CHECK(f.model.size() == 0);
}

TEST_CASE("Image.LedInputs") {
	ServiceFixture f;
	CHECK(f.controller.led_status() == hc::LedStatus::setup);
	REQUIRE(f.load(fixture("example.json")));
	CHECK(f.controller.led_status() == hc::LedStatus::degraded); // KNX datapoints, tunnel down
	f.connect();
	CHECK(f.controller.led_status() == hc::LedStatus::ready);
	REQUIRE(f.load(fixture("minimal.json")));
	CHECK(f.controller.led_status() == hc::LedStatus::ready);
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	CHECK(f.controller.led_status() == hc::LedStatus::setup);

	ServiceFixture g;
	REQUIRE(g.load(fixture("minimal.json")));
	CHECK(g.controller.led_status() == hc::LedStatus::ready); // no KNX needed
}

// ---- API ----------------------------------------------------------------------------------------

namespace {

struct ApiFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	ImageService service{controller, model, blobs, adapter, &tunnel};

	ApiFixture() {
		service.start();
		api.set_image(&service);
	}

	/// GET /api/image, reading the streamed body in pieces as a platform does.
	std::string get_image(const std::string& cookie, int& status) {
		hc::WebRequest req;
		req.method = hc::HttpMethod::get;
		req.path = "/api/image";
		req.query = "";
		req.cookie = cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		status = resp.status;
		if (!resp.stream) {
			return std::string(resp.body, resp.length);
		}
		std::string out;
		char piece[1000];
		while (out.size() < resp.stream_length) {
			size_t length = 0;
			REQUIRE(api.read_stream(resp.stream_token, out.size(), piece, sizeof(piece), length));
			REQUIRE(length > 0);
			out.append(piece, length);
		}
		return out;
	}
};

} // namespace

TEST_CASE("Image.ApiPutGetDeleteStatus") {
	ApiFixture f;
	std::string cookie = f.onboard();
	CHECK(f.api.body_limit(hc::HttpMethod::put, "/api/image") == hc::image::kMaxImageBytes);
	CHECK(f.api.body_limit(hc::HttpMethod::put, "/api/settings/tz") < hc::image::kMaxImageBytes);

	int status = 0;
	f.get_image(cookie, status);
	CHECK(status == 404);
	auto r = f.get("/api/status", cookie);
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["image"]["loaded"].boolean);

	// Without a session or the CSRF header: refused.
	std::string example = fixture("example.json");
	CHECK(f.request(hc::HttpMethod::put, "/api/image", example).status == 401);
	CHECK(f.request(hc::HttpMethod::put, "/api/image", example, cookie, "", false).status == 403);

	r = f.request(hc::HttpMethod::put, "/api/image", example, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["ok"].boolean);
	CHECK(r.json["image"]["projectId"].text == "example-house");
	CHECK(r.json["image"]["revision"].number == 7);
	CHECK(r.json["counts"]["datapoints"].number == 21);
	CHECK(r.json["counts"]["rules"].number == 2);
	CHECK(r.json["stored"].boolean);
	CHECK(r.json["source"].text == "api");

	r = f.get("/api/status", cookie);
	CHECK(r.json["image"]["loaded"].boolean);
	CHECK(r.json["image"]["project"].text == "example-house");
	CHECK(r.json["image"]["revision"].number == 7);

	CHECK(f.get_image(cookie, status) == example);
	CHECK(status == 200);

	// Refusals: 422 with the report, 400 for bad JSON; the loaded image stays.
	r = f.request(hc::HttpMethod::put, "/api/image", fixture("cases/refuse-datapoints.json"), cookie);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.invalid");
	CHECK(r.json["refusals"].items.size() == 4);
	CHECK(r.json["refusals"].items[0]["path"].text == "datapoints[0].type");
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["revision"].number == 7);
	r = f.request(hc::HttpMethod::put, "/api/image", "{", cookie);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_json");
	r = f.request(hc::HttpMethod::put, "/api/image", fixture("cases/refuse-newer-major.json"), cookie);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.version");
	r = f.get("/api/status", cookie);
	CHECK(r.json["image"]["revision"].number == 7);

	r = f.request(hc::HttpMethod::post, "/api/image", example, cookie);
	CHECK(r.status == 405);

	r = f.request(hc::HttpMethod::del, "/api/image", "", cookie);
	CHECK(r.status == 200);
	CHECK_FALSE(r.json["loaded"].boolean);
	f.get_image(cookie, status);
	CHECK(status == 404);
	CHECK(f.model.size() == 0);
}

TEST_CASE("Image.SwapResyncsTheLiveFeed") {
	ApiFixture f;
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{f.api, f.controller.loop(), transport};
	f.api.set_feed(&feed);
	std::string cookie = f.onboard();
	REQUIRE(feed.start());
	REQUIRE(feed.open(1, cookie.c_str(), nullptr, nullptr));
	std::string subscribe = "{\"type\":\"subscribe\"}";
	feed.message(1, subscribe.data(), subscribe.size());
	f.controller.loop().run_once(f.fake.clock.now_ms);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	transport.sent.clear();

	auto r = f.request(hc::HttpMethod::put, "/api/image", fixture("example.json"), cookie);
	REQUIRE(r.status == 200);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	auto status = transport.messages(1, "status");
	REQUIRE(status.size() >= 1);
	CHECK(status.back()["image"]["revision"].number == 7);
	CHECK(status.back()["datapoints"].number == 21);
	auto snapshots = transport.messages(1, "datapoints");
	REQUIRE(snapshots.size() >= 1);
	CHECK(snapshots[0]["snapshot"].boolean);
	CHECK(snapshots[0]["from"].number == 0);
	CHECK(snapshots[0]["count"].number == 21);
	// The status with the image's longest names still fits one WebSocket message.
	std::string longest = fixture("minimal.json");
	longest = replace(longest, "\"projectId\": \"minimal\"", "\"projectId\": \"" + std::string(63, 'p') +
		"\", \"name\": \"" + std::string(63, 'n') + "\"");
	REQUIRE(f.request(hc::HttpMethod::put, "/api/image", longest, cookie).status == 200);
	r = f.get("/api/status", cookie);
	MESSAGE("status with the longest image names: " << r.body.size() << " bytes");
	CHECK(r.body.size() + 100 < hc::LiveFeed::kMessageSize);

	transport.sent.clear();
	REQUIRE(f.request(hc::HttpMethod::del, "/api/image", "", cookie).status == 200);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	snapshots = transport.messages(1, "datapoints");
	REQUIRE(snapshots.size() >= 1);
	CHECK(snapshots[0]["count"].number == 0);
	CHECK(snapshots[0]["done"].boolean);
}

#if defined(__GLIBC__)
TEST_CASE("Image.MemoryFootprint") {
	// What a loaded image keeps on the heap (model tables, rule set and runtime, schedules, history
	// rings, KNX tables; the stored copy is in the blob store and counted apart), measured with
	// glibc's allocator statistics. 64-bit host: the device's pointers are half as wide.
	struct Case {
		const char* name;
		std::string text;
	} cases[] = {{"example.json", fixture("example.json")}, {"test-system template", fill_template()}};
	for (const Case& c : cases) {
		ServiceFixture f;
		size_t before = mallinfo2().uordblks;
		REQUIRE(f.load(c.text, Source::file));
		size_t after = mallinfo2().uordblks;
		size_t stored = 0;
		for (const auto& blob : f.blobs.blobs) {
			stored += blob.second.size();
		}
		Plan plan;
		Report report;
		REQUIRE(parse(c.text, plan, report));
		MESSAGE(std::string(c.name) << ": text " << c.text.size() << " B; kept after load " << (after - before)
			<< " B (blob-store copy " << stored << " B included); while loading also the text, Plan "
			<< sizeof(Plan) << " B and load tables " << plan.arena.bytes() << " B; history rings "
			<< f.service.history().bytes() << " B");
		CHECK(after > before);
	}
}
#endif

TEST_CASE("Image.ParameterDatapoints") {
	// Each rule parameter, the enable switch first, is a datapoint after the time master's
	// (spec/rules.md 8.6, spec/runtime-image.md 3.3).
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(fixture("example.json"), plan, report), describe(report));
	REQUIRE(plan.datapoint_count == 21);
	struct Expected {
		const char* name;
		ValueType type;
	};
	const Expected expected[] = {
		{"rule/hall-presence-light/enabled", ValueType::boolean},
		{"rule/hall-presence-light/timeout", ValueType::unsigned_int},
		{"rule/hall-presence-light/darkBelow", ValueType::real},
		{"rule/morning-light/enabled", ValueType::boolean},
	};
	for (size_t i = 0; i < 4; ++i) {
		const hc::DatapointSpec& spec = plan.datapoints[17 + i];
		CAPTURE(expected[i].name);
		CHECK(std::string(spec.name) == expected[i].name);
		CHECK(spec.type == expected[i].type);
		CHECK((spec.flags & DatapointFlags::kWritable) != 0);
		CHECK((spec.flags & DatapointFlags::kPersist) == 0); // the parameter store keeps them
		CHECK_FALSE(plan.technology_reads(spec.name));
	}
	CHECK(plan.technology_reads("living.temperature"));
	CHECK_FALSE(plan.technology_reads("away"));
	CHECK_FALSE(plan.technology_reads("knx.time"));
	CHECK(plan.technology_reads("not.in.the.image"));
	// A parameter that is not adjustable: its datapoint is read-only.
	Plan fixed;
	REQUIRE_MESSAGE(parse(replace(fixture("example.json"), "\"unit\": \"lx\"}", "\"unit\": \"lx\", \"adjustable\": false}"),
						fixed, report),
		describe(report));
	CHECK(std::string(fixed.datapoints[19].name) == "rule/hall-presence-light/darkBelow");
	CHECK((fixed.datapoints[19].flags & DatapointFlags::kWritable) == 0);
}

TEST_CASE("Image.ParameterDatapointRefusals") {
	auto refused = [](const std::string& text, const char* path, const char* reason) {
		Plan plan;
		Report report;
		CHECK_FALSE(parse(text, plan, report));
		bool found = false;
		for (size_t i = 0; i < report.count && !found; ++i) {
			found = std::string(report.refusals[i].path) == path && std::strstr(report.refusals[i].reason, reason) != nullptr;
		}
		CHECK_MESSAGE(found, "expected " << path << ": ..." << reason << "...\n" << describe(report));
	};
	// The prefix is reserved.
	refused(replace(fixture("example.json"), "\"id\": \"away\"", "\"id\": \"rule/away\""), "datapoints[14].id",
		"the prefix 'rule/' is reserved for the datapoints of rule parameters");
	// A read of a datapoint no technology can be asked for: the virtual one, a parameter's.
	const std::string timer_action = "\"actions\": [{\"kind\": \"restartTimer\", \"timer\": \"off\"}]";
	refused(replace(fixture("example.json"), timer_action,
				"\"actions\": [{\"kind\": \"restartTimer\", \"timer\": \"off\"}, {\"kind\": \"read\", \"datapoint\": \"away\"}]"),
		"rules[0].reactions[1].actions[1].datapoint", "notReadable");
	refused(replace(fixture("example.json"), timer_action,
				"\"actions\": [{\"kind\": \"read\", \"datapoint\": \"rule/morning-light/enabled\"}]"),
		"rules[0].reactions[1].actions[0].datapoint", "notReadable");
	// A rule's own parameter datapoint: it has the parameter.
	refused(replace(fixture("example.json"), timer_action,
				"\"actions\": [{\"kind\": \"write\", \"datapoint\": \"rule/hall-presence-light/darkBelow\", "
				"\"value\": {\"kind\": \"literal\", \"type\": \"real\", \"value\": 10.0}}]"),
		"rules[0].reactions[1].actions[0].datapoint", "forbidden");
	// A read of a KNX datapoint loads.
	Plan plan;
	Report report;
	CHECK_MESSAGE(parse(replace(fixture("example.json"), timer_action,
						  "\"actions\": [{\"kind\": \"read\", \"datapoint\": \"living.temperature\"}]"),
					  plan, report),
		describe(report));
}
