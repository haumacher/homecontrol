/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Dynamic datapoints in the image-loaded model (issue #29, doc/image.md "Dynamic datapoints"): the
// image service with a fake provider (reloads that keep values, rule state and the KNX mapping;
// refused duplicates; no image; the rate limit), the image's `zigbee` bindings in the loader, and
// the Zigbee adapter as a provider with bindings from a loaded image.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/dynamic_specs.h"
#include "hc/image/image_service.h"
#include "hc/image/loader.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/zigbee/adapter.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::DatapointFlags;
using hc::Value;
using hc::ValueType;
using hc::image::DynamicSpec;
using hc::image::ImageBindings;
using hc::image::ImageService;
using hc::image::Loader;
using hc::image::Plan;
using hc::image::Report;
using hc::image::Source;
using hc_test::Bytes;

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

bool has_refusal(const Report& report, const std::string& path, const std::string& reason) {
	for (size_t i = 0; i < report.count; ++i) {
		if (path == report.refusals[i].path && std::strstr(report.refusals[i].reason, reason.c_str()) != nullptr) {
			return true;
		}
	}
	return false;
}

/// A provider whose datapoints the test sets; a Technology that accepts every command.
class FakeProvider : public hc::image::DynamicSpecProvider, public hc::Technology {
public:
	explicit FakeProvider(const char* name) : name_(name) {}

	const char* provider_name() const override { return name_; }
	const char* name() const override { return name_; }
	bool command(const hc::Datapoint& datapoint, const Value& value) override {
		commands.push_back(std::string(datapoint.name()) + "=" + std::to_string(value.as_unsigned()));
		return true;
	}

	bool dynamic_specs(const ImageBindings& bindings, const DynamicSpec*& specs, size_t& count,
		const char** error) override {
		++asked;
		last_zigbee_bindings = bindings.zigbee_count;
		if (refuse) {
			*error = "refused by the test";
			return false;
		}
		built.clear();
		for (const std::string& name : names) {
			DynamicSpec spec;
			spec.spec = hc::DatapointSpec{name.c_str(), ValueType::unsigned_int,
				DatapointFlags::kWritable | DatapointFlags::kTransmit};
			spec.technology = this;
			built.push_back(spec);
		}
		specs = built.data();
		count = built.size();
		return true;
	}
	void model_loaded(const ImageBindings&) override { ++loaded; }
	void model_cleared() override { ++cleared; }

	/// Replaces the datapoints and tells the service.
	void set(std::vector<std::string> next) {
		names = std::move(next);
		notify_specs_changed();
	}

	std::vector<std::string> names;
	std::vector<DynamicSpec> built;
	std::vector<std::string> commands;
	int asked = 0;
	int loaded = 0;
	int cleared = 0;
	size_t last_zigbee_bindings = 0;
	bool refuse = false;

private:
	const char* name_;
};

/// A controller with the image service, a KNX tunnel and adapter, an in-memory blob store and the
/// providers registered before start (as main.cpp does).
struct DynamicFixture {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	FakeProvider provider{"fake"}; // outlives the service, which unregisters from it
	ImageService service{controller, model, blobs, adapter, &tunnel};
	size_t handled = 0;
	uint8_t gateway_sequence = 0;
	std::vector<std::string> telegrams;

	explicit DynamicFixture(bool start = true) {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		REQUIRE(service.add_provider(provider));
		if (start) {
			service.start();
		}
	}

	bool load(const std::string& text) { return service.load(text.data(), text.size(), Source::api, true); }

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

	void pump() {
		for (int round = 0; round < 100; ++round) {
			controller.run_once();
			if (handled == socket.sent.size()) {
				return;
			}
			while (handled < socket.sent.size()) {
				Bytes request = socket.sent[handled++].data;
				if (request.size() >= 8 && request[2] == 0x02 && request[3] == 0x07) {
					socket.inject(hc_test::connectionstate_response(7));
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

bool parse(const std::string& text, Plan& plan, Report& report) {
	hc_test::FakeClock clock;
	Loader loader(clock, hc::HistoryLimits().cap_bytes);
	return loader.parse(text.data(), text.size(), plan, report);
}

/// A minimal image with the given datapoints (JSON objects, comma-separated) and extra members.
std::string image_with(const std::string& datapoints, const std::string& extra = "") {
	return "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"zb\", \"revision\": 1}, \"datapoints\": [" +
		datapoints + "]" + extra + "}";
}

} // namespace

// ---- The image service with a provider ----------------------------------------------------------

TEST_CASE("Image.DynamicJoinWhileImageLoaded") {
	DynamicFixture f;
	f.connect();
	REQUIRE_MESSAGE(f.load(fixture("example.json")), describe(f.service.report()));
	f.pump();
	CHECK(f.model.size() == 21);
	CHECK(f.service.dynamic_datapoints() == 0);
	f.receive(0x0902, "00 81"); // 1/1/2 living.light.state on
	f.receive(0x0C02, "00 80 03 E8"); // 1/4/2 hall.lux 10 lx: dark
	uint64_t updated = f.dp("living.light.state").updated_ms();
	// The presence rule: presence switches the hall light on (1/1/10); presence gone starts its timer.
	f.telegrams.clear();
	f.receive(0x0C01, "00 81");
	CHECK(f.sent("09 0A 01 00 81"));
	f.receive(0x0C01, "00 80");
	f.run_for(60000);
	size_t started = f.p.sink.count("rules started");
	uint32_t reloads = f.service.dynamic_reloads(); // the start without an image counts one
	int loaded = f.provider.loaded;

	// A device joins: the model grows after the provider's change, once.
	f.provider.set({"fake.a", "fake.b"});
	f.provider.set({"fake.a", "fake.b"});
	CHECK(f.service.reload_pending());
	CHECK(f.model.size() == 21); // never within the notification
	f.run_for(0);
	CHECK(f.service.dynamic_reloads() == reloads + 1);
	REQUIRE(f.model.size() == 23);
	CHECK(f.service.dynamic_datapoints() == 2);
	CHECK(f.dp("fake.a").id() == 21);
	CHECK(f.dp("fake.a").technology() == &f.provider);
	CHECK(f.provider.loaded == loaded + 1);
	CHECK(f.p.sink.contains("model reloaded (a provider's datapoints changed): 21 datapoint(s) of the image, 2 of "
		"the providers"));
	// The image's datapoints keep their ids and values.
	CHECK(f.dp("living.light.state").id() == 1);
	CHECK(f.dp("living.light.state").value() == Value::of_bool(true));
	CHECK(f.dp("living.light.state").updated_ms() == updated);
	CHECK(f.dp("hall.light").technology() == &f.adapter);
	CHECK(f.dp("knx.time").technology() != nullptr); // the time master again
	// The KNX mapping still works both ways.
	f.receive(0x0902, "00 80");
	CHECK(f.dp("living.light.state").value() == Value::of_bool(false));
	f.telegrams.clear();
	REQUIRE(f.model.command(f.dp("living.light").id(), Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.pump();
	CHECK(f.sent("09 01 01 00 81"));
	// A provider datapoint is commanded through its technology.
	REQUIRE(f.model.command(f.dp("fake.b").id(), Value::of_unsigned(7)) == hc::DatapointStatus::ok);
	CHECK(f.provider.commands.back() == "fake.b=7");
	// The rules were not restarted: the timer started before the reload switches the light off.
	CHECK(f.p.sink.count("rules started") == started);
	f.telegrams.clear();
	f.run_for(120000);
	CHECK(f.sent("09 0A 01 00 80"));

	// The device leaves: the model shrinks (one second after the last reload at the earliest).
	f.provider.set({"fake.b"});
	f.run_for(999);
	CHECK(f.model.size() == 21 + 1);
	CHECK(f.model.find("fake.a") == nullptr);
	CHECK(f.dp("fake.b").id() == 21);
	f.provider.set({});
	f.run_for(1000);
	CHECK(f.model.size() == 21);
	CHECK(f.service.dynamic_datapoints() == 0);
	CHECK(f.service.loaded());
	CHECK(f.dp("living.light.state").value() == Value::of_bool(false));
}

TEST_CASE("Image.DynamicDuplicateNamesAreRefused") {
	DynamicFixture f;
	REQUIRE_MESSAGE(f.load(fixture("example.json")), describe(f.service.report()));
	f.provider.set({"fake.a"});
	f.run_for(999); // a second after the image's model load
	CHECK(f.model.size() == 21);
	f.run_for(1);
	REQUIRE(f.model.size() == 22);
	// A provider's name that the image has: the reload is refused, the old model stays.
	f.provider.set({"fake.a", "hall.light"});
	f.run_for(2000);
	CHECK(f.model.size() == 22);
	CHECK(f.model.find("fake.a") != nullptr);
	REQUIRE(f.service.dynamic_refusal() != nullptr);
	CHECK(std::string(f.service.dynamic_refusal()->path) == "datapoints[8].id");
	CHECK(std::string(f.service.dynamic_refusal()->reason) == "'hall.light' is also a datapoint of the provider 'fake'");
	CHECK(f.p.sink.contains("the datapoints do not compose; the model stays as it is"));
	// An image that clashes with the provider is refused with the path; the loaded one stays.
	std::string next = image_with("{\"id\": \"fake.a\", \"type\": \"unsigned\"}");
	CHECK_FALSE(f.load(next));
	CHECK(f.service.report().kind == Report::Kind::invalid);
	CHECK(has_refusal(f.service.report(), "datapoints[0].id", "'fake.a' is also a datapoint of the provider 'fake'"));
	CHECK(f.service.info().revision == 7);
	CHECK(f.model.size() == 22);
	// Two providers that need the same name.
	FakeProvider other("other");
	other.names = {"fake.a"};
	REQUIRE(f.service.add_provider(other));
	f.provider.set({"fake.a"});
	f.run_for(2000);
	REQUIRE(f.service.dynamic_refusal() != nullptr);
	CHECK(std::string(f.service.dynamic_refusal()->path).empty());
	CHECK(std::string(f.service.dynamic_refusal()->reason) ==
		"'fake.a' is a datapoint of the provider 'fake' and of the provider 'other'");
	CHECK(f.model.size() == 22);
	other.set({"other.a"});
	f.run_for(2000);
	CHECK(f.service.dynamic_refusal() == nullptr);
	CHECK(f.model.size() == 23);
	CHECK(f.dp("other.a").technology() == &other);
	REQUIRE(f.service.remove_provider(other));
	CHECK(other.cleared == 1);
	CHECK(f.model.size() == 22);
	CHECK(f.model.find("other.a") == nullptr);
	// A provider that cannot build its specs refuses the reload.
	f.provider.refuse = true;
	f.provider.set({"fake.c"});
	f.run_for(2000);
	REQUIRE(f.service.dynamic_refusal() != nullptr);
	CHECK(std::string(f.service.dynamic_refusal()->reason) ==
		"the provider 'fake' cannot build its datapoints: refused by the test");
	CHECK(f.model.size() == 22);
}

TEST_CASE("Image.DynamicWithoutImage") {
	// Registered before start and without a stored image: the providers' datapoints alone.
	DynamicFixture f(false);
	f.provider.names = {"fake.a"};
	f.provider.set({"fake.a"}); // before start: start() composes
	CHECK_FALSE(f.service.reload_pending());
	f.service.start();
	CHECK_FALSE(f.service.loaded());
	REQUIRE(f.model.size() == 1);
	CHECK(f.dp("fake.a").technology() == &f.provider);
	// A change while no image is loaded: the model follows.
	f.provider.set({"fake.a", "fake.b"});
	f.run_for(1000);
	CHECK(f.model.size() == 2);
	REQUIRE(f.model.command(f.dp("fake.a").id(), Value::of_unsigned(3)) == hc::DatapointStatus::ok);
	CHECK(f.dp("fake.a").value() == Value::of_unsigned(3));
	CHECK_FALSE(f.service.loaded());
	// An image joins them: its datapoints first, the providers' values kept.
	REQUIRE_MESSAGE(f.load(fixture("example.json")), describe(f.service.report()));
	CHECK(f.model.size() == 23);
	CHECK(f.dp("fake.a").id() == 21);
	CHECK(f.dp("fake.a").value() == Value::of_unsigned(3));
	CHECK(f.service.info().datapoints == 21);
	// Removing the image leaves the providers' datapoints.
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	CHECK_FALSE(f.service.loaded());
	CHECK(f.model.size() == 2);
	CHECK(f.dp("fake.a").id() == 0);
	CHECK(f.dp("fake.a").value() == Value::of_unsigned(3));
	// After a restart the stored image composes with the provider at boot.
	REQUIRE(f.load(fixture("minimal.json")));
	DynamicFixture h(false);
	h.blobs.blobs = f.blobs.blobs;
	h.provider.names = {"fake.z"};
	h.service.start();
	CHECK(h.service.loaded());
	CHECK(h.model.size() == 1);
	CHECK(h.dp("fake.z").technology() == &h.provider);
}

TEST_CASE("Image.DynamicReloadRateLimit") {
	DynamicFixture f;
	REQUIRE(f.load(fixture("example.json")));
	f.run_for(5000);
	uint32_t base = f.service.dynamic_reloads();
	// After a quiet second the first change reloads at once (on the next loop turn) ...
	f.provider.set({"a"});
	f.run_for(0);
	CHECK(f.service.dynamic_reloads() == base + 1);
	// ... and the changes of the following second are coalesced into one reload a second later.
	uint64_t first = f.p.clock.now_ms;
	f.run_for(100);
	f.provider.set({"a", "b"});
	f.run_for(200);
	f.provider.set({"a", "b", "c"});
	f.run_for(200);
	f.provider.set({"a", "b", "c", "d"});
	CHECK(f.service.dynamic_reloads() == base + 1);
	f.p.clock.now_ms = first + 999;
	f.run_for(0);
	CHECK(f.service.dynamic_reloads() == base + 1);
	CHECK(f.model.size() == 22);
	f.run_for(1);
	CHECK(f.service.dynamic_reloads() == base + 2);
	CHECK(f.model.size() == 25);
	// An image load composes the providers itself: a pending reload is dropped.
	f.provider.set({"a"});
	CHECK(f.service.reload_pending());
	REQUIRE(f.load(fixture("example.json")));
	CHECK_FALSE(f.service.reload_pending());
	CHECK(f.model.size() == 22);
	f.run_for(5000);
	CHECK(f.service.dynamic_reloads() == base + 2);
}

// ---- The image's zigbee bindings ------------------------------------------------------------------

TEST_CASE("Image.ZigbeeBindingsValidated") {
	const std::string ok_dp =
		"{\"id\": \"living.ceiling\", \"type\": \"boolean\", \"writable\": true, \"transmit\": true, "
		"\"zigbee\": {\"ieee\": \"00:12:4b:00:01:02:03:04\", \"endpoint\": 11, \"cluster\": 6, \"attribute\": 0, "
		"\"command\": 8}}, "
		"{\"id\": \"living.level\", \"type\": \"unsigned\", "
		"\"zigbee\": {\"ieee\": \"00124b0001020304\", \"endpoint\": 11, \"cluster\": 8, \"attribute\": 0}}, "
		"{\"id\": \"hall.button\", \"type\": \"enumeration\", "
		"\"zigbee\": {\"ieee\": \"000b57fffe112233\", \"endpoint\": 1, \"cluster\": 6, \"client\": true}}";
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(image_with(ok_dp), plan, report), describe(report));
	REQUIRE(plan.zigbee_count == 3);
	CHECK(plan.info.zigbee_datapoints == 3);
	CHECK(std::string(plan.zigbee[0].name) == "living.ceiling");
	CHECK(plan.zigbee[0].ieee == hc_test::kBulb);
	CHECK(plan.zigbee[0].endpoint == 11);
	CHECK(plan.zigbee[0].command_cluster == hc::zigbee::cluster::kLevelControl);
	CHECK(plan.zigbee[1].command_cluster == hc::zigbee::cluster::kNone);
	CHECK(plan.zigbee[2].client);
	CHECK(plan.bindings[0] == hc::image::Binding::zigbee);

	struct Case {
		std::string datapoints;
		std::string path;
		std::string reason;
	};
	const std::string zb = "\"zigbee\": {\"ieee\": \"00124b0001020304\", \"endpoint\": 11, ";
	std::vector<Case> cases = {
		{"{\"id\": \"a\", \"type\": \"boolean\", \"zigbee\": {\"ieee\": \"00124b00010203\", \"endpoint\": 1, \"cluster\": "
		 "6, \"attribute\": 0}}", "datapoints[0].zigbee.ieee", "\"00124b00010203\" is not an IEEE address"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"zigbee\": {\"ieee\": \"0000000000000000\", \"endpoint\": 1, "
		 "\"cluster\": 6, \"attribute\": 0}}", "datapoints[0].zigbee.ieee", "not 0 and not all ones"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"zigbee\": {\"endpoint\": 1, \"cluster\": 6, \"attribute\": 0}}",
		 "datapoints[0].zigbee.ieee", "ieee is missing"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"zigbee\": {\"ieee\": \"00124b0001020304\", \"endpoint\": 241, "
		 "\"cluster\": 6, \"attribute\": 0}}", "datapoints[0].zigbee.endpoint", "endpoint must be 1..240"},
		{"{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 6, \"attribute\": 5}}", "datapoints[0].zigbee",
		 "cluster 6 attribute 5: no known function for this cluster and attribute"},
		{"{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 6}}", "datapoints[0].zigbee",
		 "attribute is missing"},
		{"{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 1026, \"attribute\": 0}}", "datapoints[0].zigbee",
		 "the function temperature carries real values, the datapoint is boolean"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"transmit\": true, " + zb + "\"cluster\": 6, \"attribute\": 0}}",
		 "datapoints[0].zigbee", "a transmitting Zigbee datapoint needs the cluster its commands go to (command)"},
		{"{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 6, \"attribute\": 0, \"command\": 768}}",
		 "datapoints[0].zigbee", "the command cluster cannot set this attribute"},
		{"{\"id\": \"a\", \"type\": \"enumeration\", " + zb + "\"cluster\": 6, \"client\": true, \"command\": 6}}",
		 "datapoints[0].zigbee", "a client binding (button commands) cannot be commanded"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"knx\": {\"dpt\": \"1.001\", \"send\": \"1/1/1\"}, " + zb +
		 "\"cluster\": 6, \"attribute\": 0}}", "datapoints[0].zigbee", "a datapoint is either knx or zigbee"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"virtual\": {}, " + zb + "\"cluster\": 6, \"attribute\": 0}}",
		 "datapoints[0].zigbee", "a datapoint is either zigbee or virtual"},
		{"{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 6, \"attribute\": 0}}, "
		 "{\"id\": \"b\", \"type\": \"boolean\", " + zb + "\"cluster\": 6, \"attribute\": 0}}",
		 "datapoints[1].zigbee", "device 00124b0001020304 endpoint 11: the function on_off is bound twice (also "
		 "datapoints[0] 'a')"},
		{"{\"id\": \"a\", \"type\": \"boolean\", \"zigbee\": \"00124b0001020304\"}", "datapoints[0].zigbee",
		 "zigbee must be an object"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.datapoints);
		Plan p;
		Report r;
		CHECK_FALSE(parse(image_with(c.datapoints), p, r));
		CHECK(r.kind == Report::Kind::invalid);
		CHECK_MESSAGE(has_refusal(r, c.path, c.reason), describe(r));
	}
	// A schedule cannot own a Zigbee datapoint.
	Plan p;
	Report r;
	CHECK_FALSE(parse(image_with("{\"id\": \"a\", \"type\": \"boolean\", " + zb + "\"cluster\": 6, \"attribute\": 0}}",
		", \"schedules\": [{\"id\": \"s\", \"kind\": \"weekly\", \"time\": \"07:00\", \"output\": \"a\"}]"), p, r));
	CHECK_MESSAGE(has_refusal(r, "schedules[0].output", "outputConflict: output 'a' is owned by the Zigbee adapter"),
		describe(r));
}

// ---- The Zigbee adapter as a provider -------------------------------------------------------------

namespace {

/// The image service with the Zigbee adapter registered as its provider (main.cpp's wiring).
struct ZigbeeImageFixture {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	hc::DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter knx{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc_test::FakeBlobStore zigbee_store;
	hc_test::FakeZigbeeDriver driver;
	hc::zigbee::ZigbeeAdapter zigbee{controller.loop(), p.clock, model, driver, &zigbee_store}; // outlives the service
	ImageService service{controller, model, blobs, knx, &tunnel};
	hc_test::ZigbeeActions device{zigbee};

	ZigbeeImageFixture() {
		controller.start();
		knx.start();
		REQUIRE(service.add_provider(zigbee));
		REQUIRE(zigbee.start());
		zigbee.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
		service.start();
	}

	void run_for(uint64_t ms) {
		uint64_t until = p.clock.now_ms + ms;
		for (;;) {
			controller.run_once();
			uint64_t deadline = controller.next_deadline();
			if (deadline > until) {
				p.clock.now_ms = until;
				controller.run_once();
				return;
			}
			if (deadline > p.clock.now_ms) {
				p.clock.now_ms = deadline;
			}
		}
	}

	bool load(const std::string& text) { return service.load(text.data(), text.size(), Source::api, true); }

	const hc::Datapoint* dp(const char* name) const { return model.find(name); }
};

const char* const kSensorTemperature = "zigbee.00158d0005060708.1.temperature";
const char* const kBulbOn = "zigbee.00124b0001020304.11.on";

} // namespace

TEST_CASE("Image.ZigbeeAdapterAsProvider") {
	namespace cluster = hc::zigbee::cluster;
	namespace attribute = hc::zigbee::attribute;
	namespace zcl_type = hc::zigbee::zcl_type;
	ZigbeeImageFixture f;
	// A fresh device: no image, the first pairing works.
	CHECK_FALSE(f.service.loaded());
	f.device.join_sensor();
	f.run_for(1000);
	REQUIRE(f.dp(kSensorTemperature) != nullptr);
	CHECK(f.model.size() == 3);
	CHECK(f.dp(kSensorTemperature)->technology() == &f.zigbee);
	f.device.report(hc_test::kSensorShort, 1, cluster::kTemperatureMeasurement, attribute::kMeasuredValue,
		zcl_type::kInt16, 2150);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));

	// The image binds the bulb's On/Off (commanded through Level Control) and has a rule on it.
	std::string image = image_with(
		"{\"id\": \"living.ceiling\", \"type\": \"boolean\", \"writable\": true, \"transmit\": true, "
		"\"zigbee\": {\"ieee\": \"00124b0001020304\", \"endpoint\": 11, \"cluster\": 6, \"attribute\": 0, \"command\": 8}}, "
		"{\"id\": \"living.ceiling.copy\", \"type\": \"boolean\", \"writable\": true}",
		", \"rules\": [{\"id\": \"copy\", \"name\": \"Copy\", \"reactions\": [{\"triggers\": [{\"kind\": \"datapoint\", \"datapoint\": "
		"\"living.ceiling\"}], \"actions\": [{\"kind\": \"write\", \"datapoint\": \"living.ceiling.copy\", \"value\": "
		"{\"kind\": \"datapoint\", \"name\": \"living.ceiling\"}}]}]}]");
	REQUIRE_MESSAGE(f.load(image), describe(f.service.report()));
	f.run_for(10);
	// The image's two datapoints and the rule's enable switch (spec/rules.md 8.6), then the sensor's
	// generic ones with their values.
	REQUIRE(f.model.size() == 6);
	CHECK(f.dp("living.ceiling")->id() == 0);
	CHECK(f.dp("living.ceiling")->technology() == &f.zigbee);
	CHECK(f.dp("rule/copy/enabled")->id() == 2);
	CHECK(f.dp(kSensorTemperature)->id() == 3);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));

	// The bound bulb joins: its datapoint is the image's, no generic ones, no reload.
	uint32_t reloads = f.service.dynamic_reloads();
	f.device.join_bulb();
	f.run_for(2000);
	CHECK(f.service.dynamic_reloads() == reloads);
	CHECK(f.dp(kBulbOn) == nullptr);
	CHECK(f.driver.has("configure 0x1a2b 11 0x0006 0x0000 0..300 0"));
	f.device.report(hc_test::kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(f.dp("living.ceiling")->value() == Value::of_bool(true));
	f.run_for(10);
	CHECK(f.dp("living.ceiling.copy")->value() == Value::of_bool(true)); // the rule
	REQUIRE(f.model.command(f.dp("living.ceiling")->id(), Value::of_bool(false)) == hc::DatapointStatus::ok);
	CHECK(f.driver.calls.back() == "level 0x1a2b 11 0 5");

	// An unbound device joins: the model grows behind the image's datapoints, the rule keeps working.
	f.device.join_contact();
	f.run_for(1000);
	CHECK(f.service.dynamic_reloads() == reloads + 1);
	REQUIRE(f.dp("zigbee.00158d00090a0b0c.1.contact") != nullptr);
	CHECK(f.model.size() == 8);
	// The provider's reload kept the rule engine's binding of the parameter datapoint.
	CHECK(f.dp("rule/copy/enabled")->technology() != nullptr);
	CHECK(std::string(f.dp("rule/copy/enabled")->technology()->name()) == "rules");
	CHECK(f.dp("rule/copy/enabled")->value() == Value::of_bool(true));
	CHECK(f.dp("living.ceiling")->id() == 0);
	f.device.report(hc_test::kBulbShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	f.run_for(10);
	CHECK(f.dp("living.ceiling.copy")->value() == Value::of_bool(true));
	// The device view lists the bound name.
	char buffer[4096];
	hc::JsonWriter json(buffer, sizeof(buffer));
	f.zigbee.write_json(json);
	REQUIRE_FALSE(json.overflowed());
	std::string view(json.data(), json.length());
	CHECK(view.find("\"datapoints\":[\"living.ceiling\"]") != std::string::npos);

	// Removing the image: the generic datapoints of every device, the bulb's included.
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	CHECK(f.dp("living.ceiling") == nullptr);
	CHECK(f.dp(kBulbOn) != nullptr);
	CHECK(f.dp(kSensorTemperature)->value() == Value::of_real(21.5));
}

TEST_CASE("Image.ZigbeeRemoteIsAButtonWithItsPresses") {
	// haumacher/ehome #70, spec/runtime-image.md 3.5 (1.4): the remote's action is the enumeration press
	// of a button; every command is an update (a rule on `update` sees a second "on"), the model view
	// and so GET /api/model carry the presses for the UI.
	namespace cluster = hc::zigbee::cluster;
	ZigbeeImageFixture f;
	std::string image =
		"{\"image\": {\"schemaVersion\": \"1.4\", \"projectId\": \"zb\", \"revision\": 1}, \"datapoints\": ["
		"{\"id\": \"hall.remote\", \"type\": \"enumeration\", "
		"\"zigbee\": {\"ieee\": \"000b57fffe112233\", \"endpoint\": 1, \"cluster\": 6, \"client\": true}}, "
		"{\"id\": \"presses\", \"type\": \"unsigned\", \"writable\": true, \"virtual\": {}}], "
		"\"functions\": [{\"id\": \"remote\", \"label\": \"Hall remote\", \"kind\": \"button\", "
		"\"presses\": {\"press\": [2], \"on\": [1], \"off\": [0], \"up\": [3], \"down\": [4], \"release\": [5]}, "
		"\"datapoints\": {\"press\": \"hall.remote\"}}], "
		"\"rules\": [{\"id\": \"count\", \"name\": \"Count the on presses\", \"reactions\": [{\"triggers\": "
		"[{\"kind\": \"datapoint\", \"datapoint\": \"hall.remote\", \"on\": \"update\"}], \"condition\": "
		"{\"kind\": \"binary\", \"op\": \"==\", \"left\": {\"kind\": \"datapoint\", \"name\": \"hall.remote\"}, "
		"\"right\": {\"kind\": \"literal\", \"type\": \"integer\", \"value\": 1}}, \"actions\": [{\"kind\": \"write\", "
		"\"datapoint\": \"presses\", \"value\": {\"kind\": \"binary\", \"op\": \"+\", \"left\": {\"kind\": \"call\", "
		"\"function\": \"coalesce\", \"args\": [{\"kind\": \"datapoint\", \"name\": \"presses\"}, {\"kind\": \"literal\", "
		"\"type\": \"integer\", \"value\": 0}]}, \"right\": {\"kind\": \"literal\", \"type\": \"integer\", \"value\": 1}}}]}]}]}";
	REQUIRE_MESSAGE(f.load(image), describe(f.service.report()));
	f.device.join_button();
	f.run_for(2000);
	const hc::image::ModelView& view = f.service.view();
	REQUIRE(view.function_count() == 1);
	REQUIRE(view.function(0).presses != nullptr);
	CHECK(std::string(view.function(0).presses) ==
		"{\"press\": [2], \"on\": [1], \"off\": [0], \"up\": [3], \"down\": [4], \"release\": [5]}");
	REQUIRE(view.function(0).role_count == 1);
	CHECK(view.function(0).roles[0].datapoint == f.dp("hall.remote")->id());

	auto press = [&](uint16_t on_cluster, uint8_t command, uint8_t argument) {
		uint8_t payload[] = {argument, 0};
		f.zigbee.command_received(hc_test::ZigbeeActions::by_short(hc_test::kButtonShort), 1, on_cluster, command,
			payload, sizeof(payload));
		f.run_for(10);
	};
	press(cluster::kOnOff, 0x01, 0); // on
	CHECK(f.dp("hall.remote")->value() == Value::of_enum(1));
	press(cluster::kOnOff, 0x01, 0); // on again: the same element, another update
	CHECK(f.dp("presses")->value() == Value::of_unsigned(2));
	press(cluster::kLevelControl, 0x05, 0); // held up: a move with on/off
	CHECK(f.dp("hall.remote")->value() == Value::of_enum(3));
	press(cluster::kLevelControl, 0x07, 0); // let go: stop
	CHECK(f.dp("hall.remote")->value() == Value::of_enum(5));
	press(cluster::kOnOff, 0x00, 0); // off
	CHECK(f.dp("hall.remote")->value() == Value::of_enum(0));
	CHECK(f.dp("presses")->value() == Value::of_unsigned(2));
}

// ---- Review probe (orchestrator): a provider's datapoint across an image swap, a refused change,
// and the empty end. It reuses the fixture above, hence this file rather than a *_probe_test.cpp.
TEST_CASE("Image.DynamicProbeSwapKeepsProviderValuesAndARefusedChangeKeepsTheModel") {
	DynamicFixture f;
	REQUIRE(f.load(fixture("example.json")));
	size_t image_points = f.model.size();
	REQUIRE(image_points > 0);
	f.provider.set({"fake.x"});
	f.run_for(ImageService::kReloadSpacingMs + 500);
	const hc::Datapoint* x = f.model.find("fake.x");
	REQUIRE(x != nullptr);
	CHECK(f.model.size() == image_points + 1);
	REQUIRE(f.model.report(f.provider, x->id(), Value::of_unsigned(5)) == hc::DatapointStatus::ok);
	// An image swap to one without datapoints: the provider's point and its value survive.
	REQUIRE(f.load(fixture("minimal.json")));
	f.run_for(ImageService::kReloadSpacingMs + 500);
	x = f.model.find("fake.x");
	REQUIRE(x != nullptr);
	CHECK(f.model.size() == 1);
	CHECK(x->value() == Value::of_unsigned(5));
	// A provider that cannot build its specs: the refusal is recorded, the model stays as it is.
	f.provider.refuse = true;
	f.provider.set({"fake.x", "fake.y"});
	f.run_for(ImageService::kReloadSpacingMs + 500);
	CHECK(f.service.dynamic_refusal() != nullptr);
	CHECK(f.model.size() == 1);
	CHECK(f.model.find("fake.x") != nullptr);
	// Recovered and empty: the model empties too, and the refusal is gone.
	f.provider.refuse = false;
	f.provider.set({});
	f.run_for(ImageService::kReloadSpacingMs + 500);
	CHECK(f.model.size() == 0);
	CHECK(f.service.dynamic_refusal() == nullptr); // model_cleared() is for a cleared model, not an empty set
}

// ---- Reporting overrides of the image's zigbee bindings (spec/zigbee.md "Reporting overrides") ----

namespace {

std::string sensor_image(int revision, const std::string& temperature_reporting) {
	return "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"zb\", \"revision\": " + std::to_string(revision) +
		"}, \"datapoints\": ["
		"{\"id\": \"bath.temperature\", \"type\": \"real\", \"zigbee\": {\"ieee\": \"00158d0005060708\", \"endpoint\": 1, "
		"\"cluster\": 1026, \"attribute\": 0" + temperature_reporting + "}}, "
		"{\"id\": \"bath.humidity\", \"type\": \"real\", \"zigbee\": {\"ieee\": \"00158d0005060708\", \"endpoint\": 1, "
		"\"cluster\": 1029, \"attribute\": 0}}]}";
}

std::string export_json(const hc::zigbee::ZigbeeAdapter& adapter, size_t index) {
	char buffer[8192];
	hc::JsonWriter json(buffer, sizeof(buffer));
	adapter.write_device_export_json(json, index);
	REQUIRE_FALSE(json.overflowed());
	return std::string(json.data(), json.length());
}

} // namespace

TEST_CASE("Image.ZigbeeBindingsWithoutReportingLoadAsBefore") {
	// spec/fixtures/images/zigbee-bindings.json is an image as E-Home wrote them before `reporting`.
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(fixture("zigbee-bindings.json"), plan, report), describe(report));
	REQUIRE(plan.zigbee_count == 4);
	for (size_t i = 0; i < plan.zigbee_count; ++i) {
		CHECK(plan.zigbee[i].reporting.set == 0);
	}
	ZigbeeImageFixture f;
	REQUIRE_MESSAGE(f.load(fixture("zigbee-bindings.json")), describe(f.service.report()));
	f.device.join_sensor();
	f.device.join_bulb();
	f.run_for(2000);
	// The function table's defaults, for the bound functions only.
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0405 0x0000 30..600 100"));
	CHECK(f.driver.has("configure 0x1a2b 11 0x0006 0x0000 0..300 0"));
	CHECK(f.driver.count("configure") == 3);
	CHECK(f.zigbee.stale_timeout_s(hc_test::kSensor) == 1800);
	REQUIRE(f.dp("bath.temperature") != nullptr);
	CHECK(f.dp("bath.temperature")->technology() == &f.zigbee);
}

TEST_CASE("Image.ZigbeeReportingOverrideReachesTheDeviceAndTheExport") {
	Plan plan;
	Report report;
	REQUIRE_MESSAGE(parse(sensor_image(1, ", \"reporting\": {\"min\": 30, \"max\": 900, \"change\": 0.5}"), plan, report),
		describe(report));
	REQUIRE(plan.zigbee_count == 2);
	CHECK(plan.zigbee[0].reporting.set == (hc::zigbee::ReportingOverride::kMin | hc::zigbee::ReportingOverride::kMax |
		hc::zigbee::ReportingOverride::kChange));
	CHECK(plan.zigbee[0].reporting.min_s == 30);
	CHECK(plan.zigbee[0].reporting.max_s == 900);
	CHECK(plan.zigbee[0].reporting.change == 0.5);

	ZigbeeImageFixture f;
	REQUIRE_MESSAGE(f.load(sensor_image(1, ", \"reporting\": {\"min\": 30, \"max\": 900, \"change\": 0.5}")),
		describe(f.service.report()));
	// Loading the image configures nothing for a device that is not interviewed: its interview does.
	CHECK(f.driver.count("configure") == 0);
	f.device.join_sensor();
	f.run_for(2000);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..900 50"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0405 0x0000 30..600 100"));
	CHECK(f.driver.count("configure") == 2);
	CHECK(f.zigbee.stale_timeout_s(hc_test::kSensor) == 2700);
	std::string view = export_json(f.zigbee, 0);
	CHECK(view.find("\"reporting\":{\"min\":30,\"max\":900,\"change\":0.5},\"datapoint\":\"bath.temperature\"") !=
		std::string::npos);
	CHECK(view.find("\"reporting\":{\"min\":30,\"max\":600,\"change\":1},\"datapoint\":\"bath.humidity\"") !=
		std::string::npos);
	CHECK(view.find("\"function\":\"battery\"") != std::string::npos); // unbound, listed with its defaults
	CHECK(view.find("\"reporting\":{\"min\":3600,\"max\":21600,\"change\":1}}") != std::string::npos);

	// A new image changes the temperature's override: the interviewed sensor is configured again at
	// once, the unchanged humidity is not.
	f.driver.calls.clear();
	REQUIRE_MESSAGE(f.load(sensor_image(2, ", \"reporting\": {\"max\": 65535}")), describe(f.service.report()));
	f.run_for(10);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..65535 10"));
	CHECK(f.driver.count("configure") == 1);
	CHECK(f.zigbee.stale_timeout_s(hc_test::kSensor) == 1800); // the humidity's 600 s remain
	view = export_json(f.zigbee, 0);
	CHECK(view.find("\"reporting\":{\"min\":30,\"max\":65535,\"change\":0.1},\"datapoint\":\"bath.temperature\"") !=
		std::string::npos);

	// A refused image changes nothing on the device.
	f.driver.calls.clear();
	CHECK_FALSE(f.load(sensor_image(3, ", \"reporting\": {\"min\": 900, \"max\": 30}")));
	CHECK(has_refusal(f.service.report(), "datapoints[0].zigbee.reporting.max", "max must not be below min"));
	CHECK(f.driver.count("configure") == 0);

	// Removing the image: every function of the now generic device gets the defaults it lacks.
	const char* error = nullptr;
	REQUIRE(f.service.unload(&error));
	f.run_for(10);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0001 0x0021 3600..21600 2"));
	CHECK(f.driver.count("configure") == 2);
}

// ---- Probe: the stale rule of a device with mixed reporting, across an image swap --------------------

TEST_CASE("Image.ZigbeeReportingProbeMixedPeriodicAndOnChangeStaleRule") {
	// The temperature reports on change only (max 0, at most every 60 s); the humidity keeps its
	// periodic default. The device's stale time follows the humidity alone.
	ZigbeeImageFixture f;
	REQUIRE_MESSAGE(f.load(sensor_image(1, ", \"reporting\": {\"min\": 60, \"max\": 0}")), describe(f.service.report()));
	f.device.join_sensor();
	f.run_for(2000);
	CHECK(f.driver.has("configure 0x3c4d 1 0x0402 0x0000 60..0 10"));
	CHECK(f.driver.has("configure 0x3c4d 1 0x0405 0x0000 30..600 100"));
	CHECK(f.driver.count("configure") == 2);
	CHECK(f.zigbee.stale_timeout_s(hc_test::kSensor) == 1800);
	std::string view = export_json(f.zigbee, 0);
	CHECK(view.find("\"reporting\":{\"min\":60,\"max\":0,\"change\":0.1},\"datapoint\":\"bath.temperature\"") !=
		std::string::npos);

	f.device.report(hc_test::kSensorShort, 1, hc::zigbee::cluster::kTemperatureMeasurement,
		hc::zigbee::attribute::kMeasuredValue, hc::zigbee::zcl_type::kInt16, 2150);
	f.device.report(hc_test::kSensorShort, 1, hc::zigbee::cluster::kRelativeHumidity,
		hc::zigbee::attribute::kMeasuredValue, hc::zigbee::zcl_type::kUint16, 5500);
	f.run_for(10);
	REQUIRE(f.dp("bath.temperature") != nullptr);
	CHECK(f.dp("bath.temperature")->quality() == hc::Quality::good);
	CHECK(f.dp("bath.humidity")->quality() == hc::Quality::good);

	// Silence past three humidity maxima: both datapoints of the device are stale.
	f.run_for(1800 * 1000 + 60 * 1000);
	CHECK(f.dp("bath.temperature")->quality() == hc::Quality::stale);
	CHECK(f.dp("bath.humidity")->quality() == hc::Quality::stale);
	// A temperature report alone brings the device back; the humidity stays stale until the read the
	// revival requests is answered (doc/zigbee.md "Stale rule").
	f.driver.calls.clear();
	f.device.report(hc_test::kSensorShort, 1, hc::zigbee::cluster::kTemperatureMeasurement,
		hc::zigbee::attribute::kMeasuredValue, hc::zigbee::zcl_type::kInt16, 2200);
	f.run_for(2000); // the read-back of the revived device is scheduled, not immediate
	CHECK(f.dp("bath.temperature")->quality() == hc::Quality::good);
	CHECK(f.dp("bath.humidity")->quality() == hc::Quality::stale);
	{
		std::string calls;
		for (const std::string& call : f.driver.calls) {
			calls += call + "; ";
		}
		CHECK_MESSAGE(f.driver.has("read 0x3c4d 1 0x0405 0"), calls);
	}
	f.device.report(hc_test::kSensorShort, 1, hc::zigbee::cluster::kRelativeHumidity,
		hc::zigbee::attribute::kMeasuredValue, hc::zigbee::zcl_type::kUint16, 5600);
	f.run_for(10);
	CHECK(f.dp("bath.humidity")->quality() == hc::Quality::good);

	// The humidity goes on-change only as well: the device never goes stale again, the humidity is
	// reconfigured once, the temperature (unchanged) not.
	f.driver.calls.clear();
	std::string both = "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"zb\", \"revision\": 2}, \"datapoints\": ["
		"{\"id\": \"bath.temperature\", \"type\": \"real\", \"zigbee\": {\"ieee\": \"00158d0005060708\", \"endpoint\": 1, "
		"\"cluster\": 1026, \"attribute\": 0, \"reporting\": {\"min\": 60, \"max\": 0}}}, "
		"{\"id\": \"bath.humidity\", \"type\": \"real\", \"zigbee\": {\"ieee\": \"00158d0005060708\", \"endpoint\": 1, "
		"\"cluster\": 1029, \"attribute\": 0, \"reporting\": {\"max\": 0, \"change\": 2}}}]}";
	REQUIRE_MESSAGE(f.load(both), describe(f.service.report()));
	f.run_for(10);
	CHECK(f.driver.calls == std::vector<std::string>{"bind 0x3c4d 1 0x0405", "configure 0x3c4d 1 0x0405 0x0000 30..0 200"});
	CHECK(f.zigbee.stale_timeout_s(hc_test::kSensor) == 0);
	f.run_for(24 * 3600 * 1000);
	CHECK(f.dp("bath.temperature")->quality() == hc::Quality::good);
	CHECK(f.dp("bath.humidity")->quality() == hc::Quality::good);
	CHECK(export_json(f.zigbee, 0).find("\"reporting\":{\"min\":30,\"max\":0,\"change\":2},\"datapoint\":\"bath.humidity\"") !=
		std::string::npos);
}

// ---- Reloads without a kept plan (issue #45 item 9) ---------------------------------------------

namespace {

/// What a reload must reproduce of the image's part of the model: per datapoint its name, type,
/// flags, technology, value and quality.
struct ModelShape {
	struct Entry {
		std::string name;
		ValueType type;
		uint16_t flags;
		const hc::Technology* technology;
		Value value;
		hc::Quality quality;
	};
	std::vector<Entry> entries;
	size_t mappings = 0;
	size_t schedules = 0;
	size_t history = 0;

	static ModelShape of(DynamicFixture& f, size_t count) {
		ModelShape shape;
		for (uint16_t id = 0; id < count; ++id) {
			const hc::Datapoint& dp = *f.model.get(id);
			shape.entries.push_back({dp.name(), dp.type(), dp.flags(), dp.technology(), dp.value(), dp.quality()});
		}
		shape.mappings = f.adapter.size();
		shape.schedules = f.service.scheduler().size();
		shape.history = f.service.history().size();
		return shape;
	}
};

void check_same(const ModelShape& before, const ModelShape& after) {
	REQUIRE(before.entries.size() == after.entries.size());
	for (size_t i = 0; i < before.entries.size(); ++i) {
		const ModelShape::Entry& a = before.entries[i];
		const ModelShape::Entry& b = after.entries[i];
		CHECK_MESSAGE(a.name == b.name, "datapoint " << i);
		CHECK_MESSAGE(a.type == b.type, a.name);
		CHECK_MESSAGE(a.flags == b.flags, a.name);
		CHECK_MESSAGE(a.technology == b.technology, a.name);
		CHECK_MESSAGE(a.value == b.value, a.name);
		CHECK_MESSAGE(a.quality == b.quality, a.name);
	}
	CHECK(before.mappings == after.mappings);
	CHECK(before.schedules == after.schedules);
	CHECK(before.history == after.history);
}

} // namespace

TEST_CASE("Image.ReloadParsesTheStoredImageAgainAndKeepsNoPlan") {
	DynamicFixture f;
	f.connect();
	REQUIRE_MESSAGE(f.load(fixture("example.json")), describe(f.service.report()));
	f.pump();
	CHECK_FALSE(f.service.keeps_plan());
	CHECK(f.service.stored_ok());
	f.receive(0x0902, "00 81"); // living.light.state on
	ModelShape before = ModelShape::of(f, f.model.size());
	REQUIRE(before.entries.size() == 21);
	CHECK(before.mappings > 0);
	CHECK(before.schedules > 0);
	f.provider.set({"fake.a"});
	f.run_for(2000);
	REQUIRE(f.model.size() == 22);
	CHECK(f.service.dynamic_refusal() == nullptr);
	check_same(before, ModelShape::of(f, 21));
	CHECK_FALSE(f.service.keeps_plan());
	// And the same once more (a device leaves): the stored image is read again each time.
	f.provider.set({});
	f.run_for(2000);
	REQUIRE(f.model.size() == 21);
	check_same(before, ModelShape::of(f, 21));
	CHECK_FALSE(f.service.keeps_plan());
}

TEST_CASE("Image.ReloadKeepsThePlanOnlyForAnImageThatCannotBeReadBack") {
	SUBCASE("loaded without storing: the plan is kept, the reload uses it") {
		DynamicFixture f;
		std::string text = fixture("example.json");
		REQUIRE(f.service.load(text.data(), text.size(), Source::api, false));
		CHECK(f.service.keeps_plan());
		ModelShape before = ModelShape::of(f, f.model.size());
		f.provider.set({"fake.a"});
		f.run_for(2000);
		REQUIRE(f.model.size() == 22);
		check_same(before, ModelShape::of(f, 21));
	}
	SUBCASE("its store failed: the plan is kept from the text before it goes") {
		DynamicFixture f;
		f.blobs.fail_writes = true;
		REQUIRE(f.load(fixture("example.json")));
		CHECK_FALSE(f.service.stored_ok());
		CHECK(f.service.keeps_plan());
		CHECK(f.p.sink.contains("kept for the providers' reloads"));
		ModelShape before = ModelShape::of(f, f.model.size());
		f.blobs.fail_writes = false;
		f.provider.set({"fake.a"});
		f.run_for(2000);
		REQUIRE(f.model.size() == 22);
		check_same(before, ModelShape::of(f, 21));
	}
	SUBCASE("the stored image changed behind the loaded one: the reload refuses, saying why") {
		DynamicFixture f;
		REQUIRE(f.load(fixture("example.json")));
		CHECK_FALSE(f.service.keeps_plan());
		std::string other = image_with(R"({"id": "x", "type": "boolean"})");
		hc::image::ImageStore store(f.blobs);
		const char* error = nullptr;
		REQUIRE(store.write(other.data(), other.size(), &error));
		f.provider.set({"fake.a"});
		f.run_for(2000);
		CHECK(f.model.size() == 21);
		REQUIRE(f.service.dynamic_refusal() != nullptr);
		CHECK(std::string(f.service.dynamic_refusal()->reason).find("not the loaded one") != std::string::npos);
		CHECK(f.p.sink.contains("model not reloaded"));
		CHECK(f.service.loaded());
	}
}
