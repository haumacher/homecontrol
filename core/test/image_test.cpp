/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The binary runtime image (issues #8 and #87, spec/runtime-image.md 6 to 13): the worked example
// and its mutations, the compiled corpora, the slot (receive, boot, erase), the swap table, the
// signature on receipt, the LED inputs and the /api/image endpoints.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/image/validator.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/signature.h"
#include "hc/value_store.h"
#include "hc/web_api.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::DatapointFlags;
using hc::ValueType;
using hc::image::ImageService;
using hc::image::MemoryImageSlot;
using hc::image::Report;
using hc::image::Source;
using hc_test::Bytes;
using hc_test::describe;

namespace {

std::string fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

std::vector<uint8_t> from_hex(const char* text) {
	std::vector<uint8_t> out;
	int high = -1;
	for (const char* p = text; *p != '\0'; ++p) {
		int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
		if (v < 0) {
			continue;
		}
		if (high < 0) {
			high = v;
		} else {
			out.push_back(static_cast<uint8_t>(high << 4 | v));
			high = -1;
		}
	}
	return out;
}

/// spec/runtime-image.md 13: minimal.json compiled, 112 bytes.
const std::vector<uint8_t>& minimal() {
	static const std::vector<uint8_t> bytes = from_hex(
		"48 43 49 4d 01 00 00 00 70 00 00 00 42 2c 77 59"
		"00 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
		"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
		"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
		"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
		"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
		"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00");
	return bytes;
}

/// The same, signed with the test key "installation" (176 bytes).
const std::vector<uint8_t>& minimal_signed() {
	static const std::vector<uint8_t> bytes = from_hex(
		"48 43 49 4d 01 00 00 00 b0 00 00 00 87 ff f6 f6"
		"01 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
		"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
		"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
		"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
		"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
		"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00"
		"d8 39 e1 4f 50 d3 4b d3 97 ef 9d 57 2d d7 af 3e"
		"a8 3f d8 ce 24 a8 4e bb 28 ee ed 99 b4 1e 22 2d"
		"90 35 04 e9 07 2e 01 ac 4f 86 5c 39 3c b3 42 83"
		"fd 39 a2 ef 6e 21 aa 0f d6 7b e4 bf 92 0f 28 06");
	return bytes;
}

constexpr const char* kInstallationKey = "13833fbfa81080cdb4c1210131a5bb3e98f17675236e66124aed84cc490bccb1";
constexpr const char* kRotatedKey = "ed592207e3c3f843383c53d6d0b402b2b047323ca0eca31673655d6a940d96cd";

void put_u16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
	b[at] = static_cast<uint8_t>(v);
	b[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		b[at + i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

/// The header's CRC written anew (spec 9.2).
void fix_crc(std::vector<uint8_t>& b) {
	const auto& h = *reinterpret_cast<const hc::image::bin::Header*>(b.data());
	size_t end = (h.flags & 1) != 0 ? h.length - 64 : h.length;
	put_u32(b, 12, hc::image::bin::image_crc(b.data(), end));
}

/// All checks of a receipt on `bytes` (spec 10.1 to 10.7); the report.
bool check(const std::vector<uint8_t>& bytes, Report& report) {
	return hc::image::check_header(bytes.data(), bytes.size(), bytes.size(), report) &&
		hc::image::check_crc(bytes.data(), report) && hc::image::check_structure(bytes.data(), report);
}

bool refused_with(const Report& report, const char* code, const char* path, const char* reason) {
	if (std::string(hc::image::report_kind_code(report.kind)) != code) {
		return false;
	}
	for (size_t i = 0; i < report.count; ++i) {
		if (std::string(report.refusals[i].path) == path && std::strstr(report.refusals[i].reason, reason) != nullptr) {
			return true;
		}
	}
	return false;
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

/// A controller with the image service, a KNX tunnel and adapter on the controller's loop, a blob
/// store and an image slot in RAM (a restart: a second fixture on the same blobs and slot).
struct ServiceFixture {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore own_blobs;
	MemoryImageSlot own_slot;
	hc_test::FakeBlobStore& blobs;
	MemoryImageSlot& slot;
	ImageService service;
	size_t handled = 0;
	uint8_t gateway_sequence = 0;
	std::vector<std::string> telegrams; ///< "dest len tpdu" of each group telegram the device sent.

	explicit ServiceFixture(hc_test::FakeBlobStore* shared_blobs = nullptr, MemoryImageSlot* shared_slot = nullptr,
		const char* key = nullptr)
		: own_slot(shared_slot != nullptr ? 4096 : MemoryImageSlot::kDefaultSize),
		  blobs(shared_blobs != nullptr ? *shared_blobs : own_blobs),
		  slot(shared_slot != nullptr ? *shared_slot : own_slot),
		  service(controller, model, blobs, slot, adapter, &tunnel) {
		if (key != nullptr) {
			REQUIRE(p.settings.set_string("ehome.key", key) == hc::SettingsStatus::ok);
		}
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		service.start();
	}

	bool load(const std::string& json, Source source = Source::api) { return hc_test::deploy(service, json, source); }
	bool load_bytes(const std::vector<uint8_t>& bytes, Source source = Source::api) {
		return hc_test::deploy_bytes(service, bytes, source);
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

// ---- The worked example and its mutations (spec 13) --------------------------------------------

TEST_CASE("Image.WorkedExampleValidatesAndLoads") {
	Report report;
	CHECK_MESSAGE(check(minimal(), report), describe(report));
	CHECK(minimal().size() == 112);
	CHECK(hc::image::bin::image_crc(minimal().data(), 112) == 0x59772c42u);
	// E-Home's form of minimal.json, byte for byte.
	CHECK(hc_test::compiled(fixture("minimal.json")) == minimal());

	ServiceFixture f;
	REQUIRE_MESSAGE(f.load_bytes(minimal()), describe(f.service.report()));
	CHECK(f.service.loaded());
	CHECK(std::string(f.service.info().project_id) == "minimal");
	CHECK(std::string(f.service.info().schema_version) == "1.0");
	CHECK(f.service.info().revision == 0);
	CHECK(f.service.info().bytes == 112);
	CHECK(f.service.info().crc == 0x59772c42u);
	CHECK(f.model.size() == 0);
	// The slot holds the image as it was received; the view reads it in place.
	CHECK(std::memcmp(f.slot.bytes(), minimal().data(), 112) == 0);
	CHECK(f.service.image().bytes() == f.slot.bytes());
}

TEST_CASE("Image.SignedExampleAndTheInstallationKey") {
	Report report;
	CHECK_MESSAGE(check(minimal_signed(), report), describe(report));
	uint8_t key[hc::kEd25519KeySize];
	REQUIRE(hc::parse_public_key(kInstallationKey, key));
	CHECK(hc::image_signature_valid(reinterpret_cast<const char*>(minimal_signed().data()), 112,
		minimal_signed().data() + 112, key));

	// A device that holds the installation key takes the signed image.
	{
		ServiceFixture f(nullptr, nullptr, kInstallationKey);
		CHECK_MESSAGE(f.load_bytes(minimal_signed()), describe(f.service.report()));
		CHECK(f.service.info().is_signed);
		// The unsigned image is refused from its header: the running image stays.
		CHECK_FALSE(f.load_bytes(minimal()));
		CHECK(refused_with(f.service.report(), "image.unsigned", "header.flags",
			"the image is not signed; the device accepts only images signed with the installation key ce7d 4a41 a476 9ee3"));
		CHECK(f.service.loaded());
	}
	// Under the rotated key the signature does not verify: found after the slot was written, no image.
	{
		ServiceFixture f(nullptr, nullptr, kRotatedKey);
		REQUIRE(f.load(fixture("minimal.json")) == false); // unsigned, refused before the erase
		CHECK_FALSE(f.load_bytes(minimal_signed()));
		CHECK(refused_with(f.service.report(), "image.bad_signature", "signature",
			"the signature does not verify under the installation key 90ae 3702 f3a9 63ab"));
		CHECK_FALSE(f.service.loaded());
	}
	// Without a key a signed image loads, its signature unchecked.
	{
		ServiceFixture f;
		CHECK(f.load_bytes(minimal_signed()));
	}
}

TEST_CASE("Image.WorkedExampleMutations") {
	struct Mutation {
		const char* what;
		std::vector<uint8_t> bytes;
		const char* code;
		const char* path;
		const char* reason;
	};
	std::vector<Mutation> mutations;
	auto mutate = [](auto change, bool crc = true) {
		std::vector<uint8_t> b = minimal();
		change(b);
		if (crc) {
			fix_crc(b);
		}
		return b;
	};
	std::vector<uint8_t> cut(minimal().begin(), minimal().begin() + 100);
	mutations.push_back({"cut to 100 bytes", cut, "image.format", "header.length", "the header says 112 bytes, the body has 100"});
	std::string json = fixture("minimal.json");
	mutations.push_back({"the source text", std::vector<uint8_t>(json.begin(), json.end()), "image.format", "header.magic",
		"not a runtime image: the magic is 7B 22 69 6D, not 48 43 49 4D (\"HCIM\"): a JSON text (E-Home compiles the image; send the binary form)"});
	mutations.push_back({"major 2", mutate([](auto& b) { put_u16(b, 4, 2); }), "image.version", "header.major",
		"format 2.0: this firmware reads format 1 (up to 1.3)"});
	mutations.push_back({"a byte changed, CRC kept", mutate([](auto& b) { b[0x66] ^= 0x01; }, false), "image.damaged",
		"header.crc", "the CRC-32 is 59772c42, the bytes give"});
	mutations.push_back({"strings at 0x61", mutate([](auto& b) { put_u32(b, 0x34, 0x61); }), "image.invalid",
		"sections[1].offset", "malformed: offset 0x61 is not a multiple of 8"});
	mutations.push_back({"strings of 12 bytes", mutate([](auto& b) { put_u32(b, 0x38, 12); }), "image.invalid", "strings",
		"malformed: the last byte is not a NUL"});
	mutations.push_back({"image entry size 28", mutate([](auto& b) { put_u16(b, 0x2C, 28); }), "image.invalid",
		"sections[0].entrySize", "malformed: entry size 28 for image, which needs at least 32"});
	mutations.push_back({"strings id 0x01", mutate([](auto& b) { put_u16(b, 0x30, 0x01); }), "image.invalid", "sections[1].id",
		"malformed: section 0x01 twice"});
	mutations.push_back({"strings id 0x61 required", mutate([](auto& b) { put_u16(b, 0x30, 0x61); put_u16(b, 0x32, 1); }),
		"image.invalid", "sections[1].id", "malformed: section 0x61 is required, but this firmware does not know it"});
	mutations.push_back({"strings id 0x61 optional", mutate([](auto& b) { put_u16(b, 0x30, 0x61); }), "image.invalid",
		"sections", "malformed: the section strings (0x02) is missing"});
	mutations.push_back({"projectId 6", mutate([](auto& b) { put_u32(b, 0x40, 6); }), "image.invalid", "image[0].projectId",
		"malformed: 6 is no string (not the start of one of the 13 bytes of strings)"});
	mutations.push_back({"projectId 0", mutate([](auto& b) { put_u32(b, 0x40, 0); }), "image.invalid", "image[0].projectId",
		"malformed: empty, it is required"});
	// The image section of two records: the strings move behind it.
	std::vector<uint8_t> two = minimal();
	two.insert(two.begin() + 0x60, 32, 0);
	put_u32(two, 8, static_cast<uint32_t>(two.size()));
	put_u32(two, 0x28, 64);
	put_u32(two, 0x34, 0x80);
	fix_crc(two);
	mutations.push_back({"two image records", two, "image.invalid", "sections[0].length",
		"malformed: the section image holds 2 records, it has one"});
	for (const Mutation& m : mutations) {
		CAPTURE(m.what);
		Report report;
		CHECK_FALSE(check(m.bytes, report));
		CHECK_MESSAGE(refused_with(report, m.code, m.path, m.reason), describe(report));
		// The same through the device: never a crash, the reason as the API reports it.
		ServiceFixture f;
		CHECK_FALSE(f.load_bytes(m.bytes));
		CHECK_MESSAGE(refused_with(f.service.report(), m.code, m.path, m.reason), describe(f.service.report()));
		CHECK_FALSE(f.service.loaded());
	}

	// A newer minor with an unknown optional section: skipped, the image loads.
	std::vector<uint8_t> newer = minimal();
	put_u16(newer, 6, 1);
	put_u16(newer, 22, 3);
	newer.insert(newer.begin() + 0x40, 16, 0); // a third table entry: the sections move by 16
	put_u32(newer, 0x24, 0x50);
	put_u32(newer, 0x34, 0x70);
	size_t at = newer.size(); // the new section behind the strings (at 0x80)
	newer.resize(at + 8, 0xAB);
	put_u16(newer, 0x40, 0x61);
	put_u32(newer, 0x44, static_cast<uint32_t>(at));
	put_u32(newer, 0x48, 8);
	put_u32(newer, 8, static_cast<uint32_t>(newer.size()));
	fix_crc(newer);
	Report report;
	CHECK_MESSAGE(check(newer, report), describe(report));
	ServiceFixture f;
	CHECK_MESSAGE(f.load_bytes(newer), describe(f.service.report()));
}

// ---- The corpora (spec 13.1) ----------------------------------------------------------------------

TEST_CASE("Image.CompiledCorpus") {
	// Every image of the corpus loads: its compiled `.bin` when E-Home's compiler made one, else the
	// source compiled by the test compiler (until the corpus is committed).
	namespace fs = std::filesystem;
	size_t bins = 0;
	size_t sources = 0;
	for (const char* dir : {"", "cases/"}) {
		for (const auto& entry : fs::directory_iterator(fs::path(HC_SPEC_DIR "/fixtures/images/") / dir)) {
			std::string name = entry.path().filename().string();
			bool json = entry.path().extension() == ".json";
			if (!json || name.rfind("refuse-", 0) == 0 || name.find("template") != std::string::npos) {
				continue;
			}
			CAPTURE(name);
			fs::path bin = entry.path();
			bin.replace_extension(".bin");
			std::vector<uint8_t> bytes;
			std::string text;
			if (fs::exists(bin) && hc_test::read_text(bin.string(), text)) {
				bytes.assign(text.begin(), text.end());
				++bins;
				// E-Home's bytes equal the test compiler's (spec 9.18).
				CHECK(hc_test::compiled(fixture(std::string(dir) + name)) == bytes);
			} else {
				bytes = hc_test::compiled(fixture(std::string(dir) + name));
				++sources;
			}
			ServiceFixture f;
			CHECK_MESSAGE(f.load_bytes(bytes), describe(f.service.report()));
		}
	}
	MESSAGE("images: " << bins << " compiled by E-Home, " << sources << " by the test compiler");
	CHECK(bins + sources >= 10);

	// The mutation corpus (spec/fixtures/images/binary/): refused with the expected code, never a crash.
	fs::path mutations = HC_SPEC_DIR "/fixtures/images/binary";
	if (fs::exists(mutations)) {
		for (const auto& entry : fs::directory_iterator(mutations)) {
			if (entry.path().extension() != ".bin") {
				continue;
			}
			CAPTURE(entry.path().filename().string());
			std::string text;
			REQUIRE(hc_test::read_text(entry.path().string(), text));
			std::vector<uint8_t> bytes(text.begin(), text.end());
			fs::path expectation = entry.path();
			expectation.replace_extension(".json");
			std::string expected;
			REQUIRE(hc_test::read_text(expectation.string(), expected));
			hc_test::Json json;
			std::string parse_error;
			REQUIRE_MESSAGE(hc_test::parse_json(expected, json, parse_error), parse_error);
			// The device's key, where the outcome depends on it (spec 13.1 "deviceKey").
			const char* key = nullptr;
			if (json.has("deviceKey")) {
				key = json["deviceKey"].text == "installation" ? kInstallationKey : kRotatedKey;
			}
			ServiceFixture f(nullptr, nullptr, key);
			bool loaded = f.load_bytes(bytes);
			if (json["code"].kind == hc_test::Json::Kind::null) {
				CHECK_MESSAGE(loaded, describe(f.service.report()));
			} else {
				CHECK_FALSE(loaded);
				CHECK_MESSAGE(refused_with(f.service.report(), json["code"].text.c_str(), json["path"].text.c_str(),
								  json["reason"].text.c_str()),
					describe(f.service.report()));
			}
		}
	}
}

// ---- The slot ------------------------------------------------------------------------------------

TEST_CASE("Image.BootFromTheSlot") {
	hc_test::FakeBlobStore blobs;
	MemoryImageSlot slot;
	std::string example = fixture("example.json");
	{
		ServiceFixture f(&blobs, &slot);
		CHECK_FALSE(f.service.loaded());
		CHECK(f.service.report().ok()); // an erased slot is no image, not a refusal
		CHECK(f.p.sink.contains("no runtime image in the slot"));
		REQUIRE_MESSAGE(f.load(example), describe(f.service.report()));
		CHECK(f.model.size() == 21);
	}
	{
		ServiceFixture f(&blobs, &slot);
		CHECK(f.service.loaded());
		CHECK(f.service.source() == Source::boot);
		CHECK(f.service.info().revision == 7);
		CHECK(f.model.size() == 21);
		CHECK(f.service.engine().rules().rule_count() == 2);
		CHECK(f.p.sink.contains("loaded from boot"));
	}
	// A byte damaged in the slot: refused at boot, no image, the refusal reported.
	slot.bytes()[200] ^= 0x40;
	{
		ServiceFixture f(&blobs, &slot);
		CHECK_FALSE(f.service.loaded());
		CHECK(f.service.report().kind == Report::Kind::damaged);
		CHECK(f.model.size() == 0);
	}
	// A slot that holds something else (an old application's bytes): refused, reported.
	std::memset(slot.bytes(), 0x5A, 64);
	{
		ServiceFixture f(&blobs, &slot);
		CHECK_FALSE(f.service.loaded());
		CHECK(f.service.report().kind == Report::Kind::format);
	}
}

TEST_CASE("Image.JsonImagesOfOldFirmwareAreRemoved") {
	hc_test::FakeBlobStore blobs;
	blobs.blobs["image.idx"] = {'H', 'C', 'I', 'M', 'G'};
	blobs.blobs["image.a.0"] = {'{', '}'};
	blobs.blobs["image.b.1"] = {'{', '}'};
	blobs.blobs["values"] = {1, 2, 3};
	ServiceFixture f(&blobs);
	CHECK(blobs.blobs.count("image.idx") == 0);
	CHECK(blobs.blobs.count("image.a.0") == 0);
	CHECK(blobs.blobs.count("image.b.1") == 0);
	CHECK(blobs.blobs.count("values") == 1);
}

TEST_CASE("Image.ReceiveInPiecesAndBreakOff") {
	ServiceFixture f;
	std::vector<uint8_t> bytes = hc_test::compiled(fixture("example.json"));
	REQUIRE(bytes.size() > 4096);
	// The body in pieces of 1000 bytes, the first 32 for the header.
	REQUIRE(f.service.receive_begin(bytes.data(), 32, bytes.size(), Source::api));
	CHECK(f.service.receiving());
	CHECK_FALSE(f.service.loaded());
	for (size_t at = 0; at < bytes.size(); at += 1000) {
		REQUIRE(f.service.receive_write(bytes.data() + at, std::min<size_t>(1000, bytes.size() - at)));
	}
	REQUIRE_MESSAGE(f.service.receive_end(), describe(f.service.report()));
	CHECK(f.service.loaded());
	CHECK(f.slot.erased_sectors() == (bytes.size() + 4095) / 4096);

	// A second receive is busy while one is open.
	REQUIRE(f.service.receive_begin(bytes.data(), 32, bytes.size(), Source::api));
	CHECK_FALSE(f.service.receive_begin(bytes.data(), 32, bytes.size(), Source::api));
	CHECK(f.service.report().kind == Report::Kind::busy);
	// The body breaks off: no image, reported as image.format with the bytes received.
	REQUIRE(f.service.receive_write(bytes.data(), 4096));
	f.service.receive_abort();
	CHECK_FALSE(f.service.loaded());
	CHECK(refused_with(f.service.report(), "image.format", "header.length", "the body has 4096"));
	// The flash refuses a write: no image.
	f.slot.fail_write_after(1);
	CHECK_FALSE(f.load_bytes(bytes));
	CHECK_FALSE(f.service.loaded());
	// Larger than the slot: refused before anything changes.
	ServiceFixture small(nullptr, nullptr);
	REQUIRE(small.load_bytes(minimal()));
	std::vector<uint8_t> huge(MemoryImageSlot::kDefaultSize + 8, 0);
	std::memcpy(huge.data(), minimal().data(), 32);
	CHECK_FALSE(small.load_bytes(huge));
	CHECK(refused_with(small.service.report(), "image.too_large", "header.length",
		"the device holds at most 524288 (its image slot)"));
	CHECK(small.service.loaded());
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

	// Revision 8 drops the first datapoint (every id shifts).
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
	CHECK_FALSE(f.service.swap_kept()); // freed once the image applied

	// The presence rule runs on the new ids: presence in the dark switches the hall light (1/1/10).
	f.telegrams.clear();
	f.receive(0x0C01, "00 81"); // 1/4/1 hall.presence
	CHECK(f.sent("09 0A 01 00 81"));
	CHECK(f.dp("hall.light").value() == hc::Value::of_bool(true));
}

TEST_CASE("Image.FailedImageLeavesNoImageButKeepsTheValues") {
	ServiceFixture f;
	f.connect();
	std::string example = fixture("example.json");
	REQUIRE_MESSAGE(f.load(example), describe(f.service.report()));
	f.pump();
	f.receive(0x0902, "00 81"); // living.light.state on
	// A structurally broken image: the slot is gone, the device has no image (I9) ...
	std::vector<uint8_t> broken = hc_test::compiled(example);
	put_u32(broken, 0x40 - 0x20 + 0x24, 0x41); // the first section at an odd offset
	fix_crc(broken);
	CHECK_FALSE(f.load_bytes(broken));
	CHECK_FALSE(f.service.loaded());
	CHECK(f.service.report().kind == Report::Kind::invalid);
	CHECK(f.model.size() == 0);
	CHECK(f.service.swap_kept());
	// ... and E-Home's next deployment of the last valid image finds the values (section 12).
	REQUIRE_MESSAGE(f.load(example), describe(f.service.report()));
	CHECK(f.dp("living.light.state").value() == hc::Value::of_bool(true));
	CHECK_FALSE(f.service.swap_kept());
}

TEST_CASE("Image.TestSystemBehaviour") {
	ServiceFixture f;
	f.connect();
	std::map<std::string, uint16_t> ga;
	std::string image = fill_template(&ga);
	REQUIRE_MESSAGE(f.load(image), describe(f.service.report()));
	f.run_for(10000);
	CHECK(f.sent(hex2(ga["chA.state"]) + " 01 00 00"));
	CHECK(f.sent(hex2(ga["temperature"]) + " 01 00 00"));

	f.telegrams.clear();
	f.receive(ga["key3"], "00 81");
	CHECK(f.sent(hex2(ga["chC"]) + " 01 00 81"));
	f.receive(ga["chC.state"], "00 81");
	f.telegrams.clear();
	f.receive(ga["key3"], "00 80");
	CHECK(f.sent(hex2(ga["chC"]) + " 01 00 80"));
	f.telegrams.clear();
	f.receive(ga["key1"], "00 81");
	CHECK(f.telegrams.empty());
	CHECK(f.dp("key1").value() == hc::Value::of_bool(true));

	f.telegrams.clear();
	f.receive(ga["chD.state"], "00 81");
	f.run_for(119000);
	CHECK_FALSE(f.sent(hex2(ga["chD"]) + " 01 00 80"));
	f.run_for(1000);
	CHECK(f.sent(hex2(ga["chD"]) + " 01 00 80"));

	f.receive(ga["temperature"], "00 80 0C 1A"); // 21.0 °C
	CHECK(f.dp("temperature").value().as_real() == doctest::Approx(21.0));
	CHECK(f.dp("temperature").has_history());
	CHECK(f.dp("temperature").persists());
}

TEST_CASE("Image.UnloadErasesTheSlot") {
	ServiceFixture f;
	REQUIRE(f.load(fixture("example.json")));
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	CHECK_FALSE(f.service.loaded());
	CHECK(f.model.size() == 0);
	CHECK(f.service.engine().rules().rule_count() == 0);
	CHECK(f.service.scheduler().size() == 0);
	CHECK(f.slot.bytes()[0] == 0xFF); // erased: no image at the next start
	ServiceFixture g(&f.blobs, &f.slot);
	CHECK_FALSE(g.service.loaded());
	CHECK(g.service.report().ok());
	REQUIRE(f.load(fixture("minimal.json")));
	CHECK(f.service.loaded());
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
}

TEST_CASE("Image.ParameterDatapoints") {
	// Each rule parameter, the enable switch first, is a datapoint after the time master's
	// (spec/rules.md 8.6, spec/runtime-image.md 3.3), owned by the rule engine.
	ServiceFixture f;
	REQUIRE_MESSAGE(f.load(fixture("example.json")), describe(f.service.report()));
	REQUIRE(f.model.size() == 21);
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
		const hc::Datapoint* dp = f.model.get(static_cast<uint16_t>(17 + i));
		CAPTURE(expected[i].name);
		REQUIRE(dp != nullptr);
		CHECK(std::string(dp->name()) == expected[i].name);
		CHECK(dp->type() == expected[i].type);
		CHECK(dp->writable());
		CHECK_FALSE(dp->persists());
		REQUIRE(dp->technology() != nullptr);
		CHECK(std::string(dp->technology()->name()) == "rules");
	}
}

// ---- API ----------------------------------------------------------------------------------------

namespace {

struct ApiFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	MemoryImageSlot slot;
	ImageService service{controller, model, blobs, slot, adapter, &tunnel};

	ApiFixture() {
		service.start();
		api.set_image(&service);
	}

	/// GET `path`, reading a streamed body in pieces as a platform does.
	std::string get_stream(const std::string& path, const std::string& cookie, int& status) {
		hc::WebRequest req;
		req.method = hc::HttpMethod::get;
		req.path = path.c_str();
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

std::string as_text(const std::vector<uint8_t>& bytes) {
	return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("Image.ApiPutGetDeleteStatus") {
	ApiFixture f;
	std::string cookie = f.onboard();
	CHECK(f.api.body_limit(hc::HttpMethod::put, "/api/image") == MemoryImageSlot::kDefaultSize);
	char text[128];
	CHECK(std::string(f.api.body_too_long(hc::HttpMethod::put, "/api/image", 600000, text, sizeof(text))) ==
		"image.too_large");
	CHECK(std::string(text) == "the image has 600000 bytes; the device holds at most 524288 (its image slot)");

	int status = 0;
	f.get_stream("/api/image", cookie, status);
	CHECK(status == 404);
	auto r = f.get("/api/status", cookie);
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["image"]["loaded"].boolean);
	CHECK(r.json["image"]["slot"].number == 524288);
	CHECK(r.json["device"]["format"].text == "1.3");

	std::string example = as_text(hc_test::compiled(fixture("example.json")));
	CHECK(f.request(hc::HttpMethod::put, "/api/image", example).status == 401);
	CHECK(f.request(hc::HttpMethod::put, "/api/image", example, cookie, "", false).status == 403);

	r = f.request(hc::HttpMethod::put, "/api/image", example, cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["ok"].boolean);
	CHECK(r.json["image"]["projectId"].text == "example-house");
	CHECK(r.json["image"]["revision"].number == 7);
	CHECK(r.json["image"]["format"].text == "1.0");
	CHECK(r.json["image"]["bytes"].number == example.size());
	CHECK(r.json["counts"]["datapoints"].number == 21);
	CHECK(r.json["counts"]["rules"].number == 2);
	CHECK(r.json["source"].text == "api");

	r = f.get("/api/status", cookie);
	CHECK(r.json["image"]["loaded"].boolean);
	CHECK(r.json["image"]["project"].text == "example-house");
	CHECK(r.json["image"]["revision"].number == 7);
	CHECK(r.json["image"]["format"].text == "1.0");
	CHECK(r.json["image"]["crc"].text.size() == 8);

	CHECK(f.get_stream("/api/image", cookie, status) == example);
	CHECK(status == 200);

	// Refused from the header: the running image stays (loaded true, its revision).
	std::string json = fixture("minimal.json");
	r = f.request(hc::HttpMethod::put, "/api/image", json, cookie);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.format");
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["revision"].number == 7);
	std::vector<uint8_t> major = minimal();
	put_u16(major, 4, 2);
	r = f.request(hc::HttpMethod::put, "/api/image", as_text(major), cookie);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.version");
	// A detached signature is refused.
	f.image_signature = std::string(128, 'a');
	r = f.request(hc::HttpMethod::put, "/api/image", example, cookie);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_request");
	f.image_signature.clear();
	// Refused after the slot was written: no image, the status says why.
	std::vector<uint8_t> damaged = minimal();
	damaged[0x66] ^= 1;
	r = f.request(hc::HttpMethod::put, "/api/image", as_text(damaged), cookie);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.damaged");
	CHECK_FALSE(r.json["loaded"].boolean);
	r = f.get("/api/status", cookie);
	CHECK_FALSE(r.json["image"]["loaded"].boolean);
	CHECK(r.json["image"]["refused"].text == "image.damaged");
	CHECK(r.json["image"]["reason"].text.find("header.crc: the CRC-32 is") == 0);
	f.get_stream("/api/image", cookie, status);
	CHECK(status == 404);

	r = f.request(hc::HttpMethod::put, "/api/image", example, cookie);
	REQUIRE(r.status == 200);
	r = f.request(hc::HttpMethod::post, "/api/image", example, cookie);
	CHECK(r.status == 405);
	r = f.request(hc::HttpMethod::del, "/api/image", "", cookie);
	CHECK(r.status == 200);
	CHECK_FALSE(r.json["loaded"].boolean);
	f.get_stream("/api/image", cookie, status);
	CHECK(status == 404);
	CHECK(f.model.size() == 0);
}

TEST_CASE("Image.ApiPagesServesTheUiSection") {
	ApiFixture f;
	std::string cookie = f.onboard();
	auto r = f.get("/api/pages", cookie);
	CHECK_FALSE(r.json["loaded"].boolean);
	REQUIRE(f.request(hc::HttpMethod::put, "/api/image", as_text(hc_test::compiled(fixture("minimal.json"))), cookie)
				.status == 200);
	r = f.get("/api/pages", cookie);
	CHECK(r.body == "{\"pages\":[],\"sections\":[],\"elements\":[],\"series\":[],\"references\":[]}");
	REQUIRE(f.request(hc::HttpMethod::put, "/api/image", as_text(hc_test::compiled(fixture("pages.json"))), cookie)
				.status == 200);
	int status = 0;
	std::string body = f.get_stream("/api/pages", cookie, status);
	CHECK(status == 200);
	size_t length = 0;
	const char* ui = f.service.image().ui(length);
	REQUIRE(ui != nullptr);
	CHECK(body == std::string(ui, length));
	hc_test::Json pages;
	std::string parse_error;
	REQUIRE_MESSAGE(hc_test::parse_json(body, pages, parse_error), parse_error);
	CHECK(pages["pages"].items.size() > 0);
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

	auto r = f.request(hc::HttpMethod::put, "/api/image", as_text(hc_test::compiled(fixture("example.json"))), cookie);
	REQUIRE(r.status == 200);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	auto status = transport.messages(1, "status");
	REQUIRE(status.size() >= 1);
	CHECK(status.back()["image"]["revision"].number == 7);
	CHECK(status.back()["datapoints"].number == 21);
	auto snapshots = transport.messages(1, "datapoints");
	REQUIRE(snapshots.size() >= 1);
	CHECK(snapshots.back()["snapshot"].boolean);
	CHECK(snapshots.back()["count"].number == 21);
	// The status with the image's longest names still fits one WebSocket message.
	std::string longest = replace(fixture("minimal.json"), "\"projectId\": \"minimal\"",
		"\"projectId\": \"" + std::string(63, 'p') + "\", \"name\": \"" + std::string(63, 'n') + "\"");
	REQUIRE(f.request(hc::HttpMethod::put, "/api/image", as_text(hc_test::compiled(longest)), cookie).status == 200);
	r = f.get("/api/status", cookie);
	MESSAGE("status with the longest image names: " << r.body.size() << " bytes");
	CHECK(r.body.size() + 100 < hc::LiveFeed::kMessageSize);

	transport.sent.clear();
	REQUIRE(f.request(hc::HttpMethod::del, "/api/image", "", cookie).status == 200);
	f.controller.loop().run_once(f.fake.clock.now_ms);
	snapshots = transport.messages(1, "datapoints");
	REQUIRE(snapshots.size() >= 1);
	CHECK(snapshots.back()["count"].number == 0);
}

#if defined(__GLIBC__)
#include <malloc.h>
TEST_CASE("Image.MemoryFootprint") {
	// What a loaded image keeps on the heap (the slot is the platform's, allocated before), measured
	// with glibc's statistics on the 64-bit host; the compiled bytes are made before the measurement.
	struct Case {
		const char* name;
		std::string text;
	} cases[] = {{"example.json", fixture("example.json")}, {"test-system template", fill_template()}};
	for (const Case& c : cases) {
		std::vector<uint8_t> bytes = hc_test::compiled(c.text);
		ServiceFixture f;
		size_t before = mallinfo2().uordblks;
		REQUIRE(f.load_bytes(bytes, Source::file));
		size_t after = mallinfo2().uordblks;
		MESSAGE(c.name << ": image " << bytes.size() << " B; kept after load " << (after - before) << " B (history rings "
			<< f.service.history().bytes() << " B, swap table kept: " << f.service.swap_kept() << ")");
		CHECK_FALSE(f.service.swap_kept());
	}
}
#endif

namespace {

/// GET `path` with the query into a response buffer of `capacity` bytes, as a platform with that
/// buffer (8 KB on the device and the POSIX server, 4 KB through the E-Home connection) does.
hc_test::WebFixture::Result get_into(ApiFixture& f, const char* path, const std::string& query, const std::string& cookie,
	size_t capacity) {
	std::vector<char> buffer(capacity);
	hc::WebRequest req;
	req.method = hc::HttpMethod::get;
	req.path = path;
	req.query = query.c_str();
	req.cookie = cookie.c_str();
	req.requested_with = "HomeControl";
	req.source = "192.0.2.7";
	hc::WebResponse resp(buffer.data(), buffer.size());
	f.api.handle(req, resp);
	hc_test::WebFixture::Result r;
	r.status = resp.status;
	r.body.assign(resp.body, resp.length);
	std::string error;
	hc_test::JsonReader(r.body).parse(r.json, error);
	return r;
}

} // namespace

TEST_CASE("Image.ApiModelPagesAtEveryBoundary") {
	// A page of GET /api/model filled up to the closing reserve still closes its lists (found on a
	// controller: the page whose last entry ended exactly at the reserve was refused with
	// api.response_too_large, depending on the length of the live numbers in its head). Every page
	// start, buffers of every size around the entries' lengths, every image with a model.
	for (const char* name : {"example.json", "pages.json", "zigbee-bindings.json", "heating-timeline.json"}) {
		CAPTURE(name);
		ApiFixture f;
		std::string cookie = f.onboard();
		REQUIRE(f.request(hc::HttpMethod::put, "/api/image", as_text(hc_test::compiled(fixture(name))), cookie).status == 200);
		auto first = get_into(f, "/api/model", "from=0", cookie, 8192);
		REQUIRE(first.status == 200);
		size_t total = static_cast<size_t>(first.json["total"].number);
		size_t pages = 0;
		for (size_t capacity = 700; capacity <= 1400; ++capacity) {
			for (size_t from = 0; from < total; ++from) {
				std::string query = "from=" + std::to_string(from);
				auto r = get_into(f, "/api/model", query, cookie, capacity);
				if (r.status == 500 && r.body.find("an entry of the model does not fit") != std::string::npos) {
					continue; // one entry larger than this buffer: refused as such
				}
				CAPTURE(capacity);
				CAPTURE(from);
				REQUIRE_MESSAGE(r.status == 200, r.body);
				REQUIRE_MESSAGE(r.json.kind == hc_test::Json::Kind::object, r.body);
				++pages;
			}
		}
		CHECK(pages > 0);
	}
}
