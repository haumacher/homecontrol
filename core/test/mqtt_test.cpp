/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/loader.h"
#include "hc/mqtt.h"
#include "mqtt_test_support.h"
#include "test_support.h"

#ifndef HC_SPEC_DIR
#error "HC_SPEC_DIR must name the spec directory"
#endif

using hc::DatapointStatus;
using hc::Mqtt;
using hc::MqttState;
using hc::MqttStatus;
using hc::Value;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::FakeMqttClient;
using hc_test::FakeNetworkDriver;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;

namespace {

constexpr int64_t kWall0 = 1792270800; // 2026-10-17T12:20:00Z

std::string read_spec(const std::string& path) {
	std::ifstream in(std::string(HC_SPEC_DIR) + "/" + path, std::ios::binary);
	REQUIRE_MESSAGE(in.good(), path);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

/// A technology that only reports (a sensor on a bus).
class Sensor : public hc::Technology {
public:
	const char* name() const override { return "sensor"; }
	bool command(const hc::Datapoint&, const Value&) override { return false; }
};

/// The `mqtt` member a test adds to an image without one.
const char* const kMember = "{\"broker\": \"mqtt://broker.example:1883\"}";

struct Fixture {
	ScopedCapture capture{hc::LogLevel::debug};
	FakeClock clock;
	hc::EventLoop loop;
	FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	FakeSettings settings;
	hc::DatapointModel model{clock};
	FakeBlobStore blobs;
	hc::History history{loop, clock, model, blobs};
	FakeMqttClient client;
	std::unique_ptr<Mqtt> mqtt;
	uint64_t mono0;

	Fixture() {
		clock.wall_seconds = kWall0;
		clock.synchronized = true;
		mono0 = clock.now_ms;
		connectivity.start();
		connectivity.link_up("192.0.2.100");
		hc::MqttIdentity identity;
		identity.node_id = "homecontrol-a1b2c3";
		identity.sw_version = "test";
		mqtt.reset(new Mqtt(loop, clock, settings, connectivity, model, client, identity, &history));
	}

	/// Loads an image's datapoints and history into the model (what the image service does first).
	void load_model(const std::string& text) {
		hc::image::Plan plan;
		hc::image::Report report;
		hc::image::Loader loader(clock, hc::HistoryLimits().cap_bytes);
		REQUIRE_MESSAGE(loader.parse(text.data(), text.size(), plan, report),
			(report.count > 0 ? std::string(report.refusals[0].path) + ": " + report.refusals[0].reason : ""));
		REQUIRE(model.load(plan.datapoints, plan.datapoint_count, true) == DatapointStatus::ok);
		REQUIRE(history.load(plan.history, plan.history_count, plan.history_budget) == hc::HistoryStatus::ok);
	}

	/// Loads an image and configures the client from it; `member` replaces (or adds) its `mqtt` member.
	void load(const std::string& text, const char* member = nullptr) {
		load_model(text);
		const char* error = nullptr;
		if (member == nullptr) {
			REQUIRE_MESSAGE(mqtt->configure_image(text.data(), text.size(), &error), (error ? error : ""));
			return;
		}
		hc::JsonSpan root;
		REQUIRE(hc::json_parse(text.data(), text.size(), root, nullptr, hc::kJsonMaxDepth));
		hc::MqttImage image;
		hc::json_member(root, "datapoints", image.datapoints);
		hc::json_member(root, "functions", image.functions);
		hc::JsonSpan info;
		if (hc::json_member(root, "image", info)) {
			hc::json_member(info, "name", image.name);
		}
		member_text = member;
		REQUIRE(hc::json_parse(member_text.data(), member_text.size(), image.mqtt));
		REQUIRE_MESSAGE(mqtt->configure(image, &error), (error ? error : ""));
	}

	void sync_wall() { clock.wall_seconds = kWall0 + static_cast<int64_t>((clock.now_ms - mono0) / 1000); }

	/// Runs the loop, jumping the clock from deadline to deadline, until `until_ms`.
	void run_until(uint64_t until_ms) {
		for (;;) {
			sync_wall();
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				sync_wall();
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	/// Runs the work due now (posted callbacks and what they post).
	void settle() {
		for (int i = 0; i < 100 && loop.next_deadline() <= clock.now_ms; ++i) {
			loop.run_once(clock.now_ms);
		}
	}

	/// Connects and lets the session's work run; clears the record of publications.
	void connect() {
		settle();
		REQUIRE(client.session);
		client.accept();
		settle();
	}

	const hc::Datapoint& dp(const char* name) {
		const hc::Datapoint* d = model.find(name);
		REQUIRE(d != nullptr);
		return *d;
	}

	void command(const char* name, const Value& value) { REQUIRE(model.command(dp(name).id(), value) == DatapointStatus::ok); }

	void bind(hc::Technology& technology, const char* name) { REQUIRE(model.bind(dp(name).id(), technology) == DatapointStatus::ok); }

	void report(hc::Technology& technology, const char* name, const Value& value) {
		REQUIRE(model.report(technology, dp(name).id(), value) == DatapointStatus::ok);
	}

	std::string member_text;
};

std::string example() { return read_spec("fixtures/images/example.json"); }
std::string kinds() { return read_spec("fixtures/mqtt/kinds.json"); }

/// The discovery configs as "<topic> <payload>" lines, in publication order.
std::string discovery_lines(const FakeMqttClient& client, const std::string& prefix) {
	std::string out;
	for (const auto& p : client.under(prefix + "/")) {
		if (p.payload.empty()) {
			continue;
		}
		CHECK(p.retain);
		CHECK(p.qos == 1);
		out += p.topic + " " + p.payload + "\n";
	}
	return out;
}

/// Compares with a fixture; with HC_WRITE_FIXTURES set in the environment it writes the fixture.
void check_snapshot(const std::string& actual, const std::string& fixture) {
	if (std::getenv("HC_WRITE_FIXTURES") != nullptr) {
		std::ofstream out(std::string(HC_SPEC_DIR) + "/" + fixture, std::ios::binary);
		out << actual;
		return;
	}
	std::string expected = read_spec(fixture);
	std::istringstream a(actual), e(expected);
	std::string la, le;
	int line = 0;
	while (true) {
		bool ga = static_cast<bool>(std::getline(a, la));
		bool ge = static_cast<bool>(std::getline(e, le));
		++line;
		if (!ga && !ge) {
			break;
		}
		INFO(fixture << " line " << line);
		CHECK(la == le);
		if (la != le) {
			break;
		}
	}
}

} // namespace

TEST_SUITE("Mqtt") {

TEST_CASE("MqttConnectsWithTheConfigurationAndAnnouncesItself") {
	Fixture f;
	f.settings.set_string("mqtt.password", "secret-pw");
	f.load(kinds());
	f.settle();
	REQUIRE(f.client.connects == 1);
	CHECK(f.mqtt->state() == MqttState::connecting);
	CHECK(f.client.broker == "mqtt://broker.example:1883");
	CHECK(f.client.client_id == "homecontrol-a1b2c3");
	CHECK(f.client.username == "homecontrol");
	CHECK(f.client.password == "secret-pw");
	CHECK(f.client.will_topic == "house/main/status");
	CHECK(f.client.will_payload == "offline");
	CHECK(f.client.will_retain);
	CHECK(f.client.will_qos == 1);
	CHECK_FALSE(f.capture.sink.contains("secret-pw"));

	f.connect();
	CHECK(f.mqtt->state() == MqttState::connected);
	auto status = f.client.to("house/main/status");
	REQUIRE(status.size() == 1);
	CHECK(status[0].payload == "online");
	CHECK(status[0].retain);
	CHECK(f.client.published[0].topic == "house/main/status"); // first of all
	CHECK(f.client.subscribed("house/main/dp/+/set"));
	CHECK(f.client.subscribed("house/main/fn/+/set"));
	CHECK(f.client.subscribed("ha/+/homecontrol-a1b2c3/+/config"));
	CHECK(f.client.subscribed("house/main/dp/+/state"));
	CHECK(f.client.under("ha/").size() == 24); // one config per function
	CHECK(f.capture.sink.contains("connected to mqtt://broker.example:1883"));
}

TEST_CASE("MqttDefaultBaseTopicAndUserFromTheSettings") {
	Fixture f;
	f.settings.set_string("mqtt.user", "ha-user");
	f.load(example(), kMember);
	f.connect();
	CHECK(f.client.username == "ha-user");
	CHECK(f.client.will_topic == "homecontrol/a1b2c3/status");
	CHECK(f.client.to("homecontrol/a1b2c3/status").size() == 1);
	CHECK(f.client.under("homeassistant/").size() == 9);
}

TEST_CASE("MqttWaitsForTheNetwork") {
	Fixture f;
	f.connectivity.link_down("test");
	f.load(example(), kMember);
	f.settle();
	CHECK(f.client.connects == 0);
	CHECK(f.mqtt->state() == MqttState::waiting);
	f.run_for(Mqtt::kReadyPollMs * 3);
	CHECK(f.client.connects == 0);
	f.connectivity.link_up("192.0.2.100");
	f.run_for(Mqtt::kReadyPollMs);
	CHECK(f.client.connects == 1);
}

TEST_CASE("MqttBacksOffAfterFailuresAndResetsOnSuccess") {
	Fixture f;
	f.load(example(), kMember);
	f.settle();
	REQUIRE(f.client.connects == 1);
	uint64_t t0 = f.clock.now_ms;
	f.client.drop("connection refused: not authorized");
	CHECK(f.mqtt->state() == MqttState::backoff);
	CHECK(f.capture.sink.contains("cannot connect: connection refused: not authorized; retry in 5 s (failure 1)"));
	f.run_until(t0 + 4999);
	CHECK(f.client.connects == 1);
	f.run_until(t0 + 5000);
	CHECK(f.client.connects == 2);
	f.client.drop("x");
	f.run_for(9999);
	CHECK(f.client.connects == 2);
	f.run_for(1);
	CHECK(f.client.connects == 3);
	// No answer at all: the attempt times out after 30 s and counts as a failure (backoff 20 s).
	f.run_for(Mqtt::kConnectTimeoutMs);
	CHECK(f.client.disconnects == 1);
	CHECK(f.capture.sink.contains("no answer from the broker within 30 s; retry in 20 s (failure 3)"));
	f.run_for(20000);
	CHECK(f.client.connects == 4);
	f.client.accept();
	MqttStatus status;
	f.mqtt->status(status);
	CHECK(status.failures == 0);
	CHECK(status.connects == 1);
	// A lost connection starts over at 5 s.
	f.client.drop("keepalive timeout");
	CHECK(f.capture.sink.contains("connection lost: keepalive timeout; retry in 5 s (failure 1)"));
	f.run_for(5000);
	CHECK(f.client.connects == 5);
}

TEST_CASE("MqttBackoffCapsAtFiveMinutes") {
	Fixture f;
	f.client.refuse_connect = "TLS is not supported on this platform";
	f.load(example(), kMember);
	f.settle();
	CHECK(f.capture.sink.contains("TLS is not supported on this platform; retry in 5 s (failure 1)"));
	f.run_for(3600000);
	CHECK(f.capture.sink.contains("retry in 300 s"));
	CHECK_FALSE(f.capture.sink.contains("retry in 600 s"));
}

TEST_CASE("MqttReconnectRepublishesTheStates") {
	Fixture f;
	f.load(example(), kMember);
	f.command("away", Value::of_bool(true));
	f.connect();
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/state").size() == 1);
	f.client.drop("network");
	f.run_for(5000);
	f.client.accept();
	f.settle();
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/state").size() == 2);
	CHECK(f.client.to("homecontrol/a1b2c3/status").size() == 2);
}

TEST_CASE("MqttStatePayloadAndRetain") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	f.clock.now_ms += 3000;
	f.sync_wall();
	f.command("living.setpoint", Value::of_real(21.5));
	f.settle();
	auto p = f.client.to("homecontrol/a1b2c3/dp/living.setpoint/state");
	REQUIRE(p.size() == 1);
	CHECK(p[0].payload == "{\"value\":21.5,\"quality\":\"good\",\"time\":" + std::to_string(f.clock.wall_seconds) + "}");
	CHECK(p[0].retain);
	CHECK(p[0].qos == 0);
	// Without a synchronized clock the time is left out; with retain false the state is not retained.
	f.clock.synchronized = false;
	f.command("living.setpoint", Value::of_real(22));
	f.settle();
	p = f.client.to("homecontrol/a1b2c3/dp/living.setpoint/state");
	REQUIRE(p.size() == 2);
	CHECK(p[1].payload == "{\"value\":22,\"quality\":\"good\"}");
	f.load(example(), "{\"broker\": \"mqtt://broker.example:1883\", \"retain\": false}");
	f.settle();
	f.command("living.setpoint", Value::of_real(23));
	f.settle();
	p = f.client.to("homecontrol/a1b2c3/dp/living.setpoint/state");
	CHECK_FALSE(p.back().retain);
}

TEST_CASE("MqttCoalescesABurstWithinOneTurn") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	f.command("living.setpoint", Value::of_real(19));
	f.command("living.setpoint", Value::of_real(20));
	f.command("living.setpoint", Value::of_real(21));
	f.command("away", Value::of_bool(true));
	f.settle();
	auto p = f.client.to("homecontrol/a1b2c3/dp/living.setpoint/state");
	REQUIRE(p.size() == 1);
	CHECK(p[0].payload.find("\"value\":21,") != std::string::npos);
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/state").size() == 1);
	CHECK(f.client.published.size() == 2);
	// A repeated value is no change.
	f.command("away", Value::of_bool(true));
	f.settle();
	CHECK(f.client.published.size() == 2);
}

TEST_CASE("MqttFullQueueKeepsTheLatestStateAndRetries") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	f.client.capacity = f.client.queued; // full
	f.command("living.setpoint", Value::of_real(19));
	f.settle();
	CHECK(f.client.published.empty());
	f.command("living.setpoint", Value::of_real(25));
	f.run_for(Mqtt::kRetryPublishMs * 3);
	CHECK(f.client.published.empty());
	MqttStatus status;
	f.mqtt->status(status);
	CHECK(status.dirty == 1);
	f.client.queued = 0; // drained
	f.run_for(Mqtt::kRetryPublishMs);
	auto p = f.client.to("homecontrol/a1b2c3/dp/living.setpoint/state");
	REQUIRE(p.size() == 1);
	CHECK(p[0].payload.find("\"value\":25,") != std::string::npos);
}

TEST_CASE("MqttBurstOfManyDatapointsIsSpreadOverTurns") {
	Fixture f;
	std::string text = "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"burst\", \"revision\": 1}, \"datapoints\": [";
	for (int i = 0; i < 100; ++i) {
		text += std::string(i > 0 ? "," : "") + "{\"id\": \"v" + std::to_string(i) + "\", \"type\": \"real\", \"writable\": true}";
	}
	text += "], \"mqtt\": {\"broker\": \"mqtt://b.example\", \"history\": {\"enabled\": false}}}";
	f.load(text);
	f.connect();
	f.client.published.clear();
	for (int i = 0; i < 100; ++i) {
		f.command(("v" + std::to_string(i)).c_str(), Value::of_real(i));
	}
	f.loop.run_once(f.clock.now_ms);
	CHECK(f.client.published.size() == Mqtt::kPublishesPerTurn); // one turn's share
	f.settle();
	CHECK(f.client.published.size() == 100);
}

TEST_CASE("MqttCommandsReachTheModel") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	f.client.deliver("homecontrol/a1b2c3/dp/living.setpoint/set", "21.5");
	CHECK(f.dp("living.setpoint").value() == Value::of_real(21.5));
	f.client.deliver("homecontrol/a1b2c3/dp/away/set", "true");
	CHECK(f.dp("away").value() == Value::of_bool(true));
	f.settle();
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/state").size() == 1);
	MqttStatus status;
	f.mqtt->status(status);
	CHECK(status.commands == 2);
	CHECK(status.refusals == 0);
}

TEST_CASE("MqttCommandRefusalsArePublished") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	struct Case {
		const char* name;
		const char* payload;
		const char* error;
	};
	const Case cases[] = {
		{"hall.presence", "true", "{\"error\":\"the datapoint is not writable\",\"payload\":\"true\"}"},
		{"away", "on", "{\"error\":\"the payload is not a JSON value: "},
		{"away", "1.5", "{\"error\":\"not a boolean value: "},
		{"no.such", "1", "{\"error\":\"unknown datapoint\",\"payload\":\"1\"}"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.name);
		CAPTURE(c.payload);
		f.client.deliver(std::string("homecontrol/a1b2c3/dp/") + c.name + "/set", c.payload);
		auto e = f.client.to(std::string("homecontrol/a1b2c3/dp/") + c.name + "/error");
		REQUIRE_FALSE(e.empty());
		CHECK(e.back().payload.compare(0, std::strlen(c.error), c.error) == 0);
		CHECK_FALSE(e.back().retain);
	}
	CHECK_FALSE(f.dp("away").value() == Value::of_bool(true));
	// Retained and truncated commands are refused.
	f.client.deliver("homecontrol/a1b2c3/dp/away/set", "true", true);
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/error").back().payload.find("retained commands are ignored") !=
		std::string::npos);
	f.client.deliver("homecontrol/a1b2c3/dp/away/set", "true", false, true);
	CHECK(f.client.to("homecontrol/a1b2c3/dp/away/error").back().payload.find("too long") != std::string::npos);
	CHECK(f.dp("away").value().empty());
	MqttStatus status;
	f.mqtt->status(status);
	CHECK(status.refusals == 6);
	CHECK(f.capture.sink.contains("command on homecontrol/a1b2c3/dp/hall.presence/set refused: the datapoint is not writable"));
}

TEST_CASE("MqttBlindTakesOpenCloseStop") {
	Fixture f;
	f.load(kinds());
	f.connect();
	f.client.deliver("house/main/fn/blind/set", "CLOSE");
	CHECK(f.dp("blind.move").value() == Value::of_bool(true));
	f.client.deliver("house/main/fn/blind/set", "OPEN");
	CHECK(f.dp("blind.move").value() == Value::of_bool(false));
	f.client.deliver("house/main/fn/blind/set", "STOP");
	CHECK(f.dp("blind.stop").value() == Value::of_bool(true));
	f.client.deliver("house/main/fn/blind/set", "UP");
	CHECK(f.client.to("house/main/fn/blind/error").back().payload.find("OPEN, CLOSE or STOP") != std::string::npos);
	f.client.deliver("house/main/fn/light/set", "OPEN");
	CHECK(f.client.to("house/main/fn/light/error").back().payload.find("only a blind") != std::string::npos);
	f.client.deliver("house/main/fn/nothing/set", "OPEN");
	CHECK(f.client.to("house/main/fn/nothing/error").back().payload.find("unknown function") != std::string::npos);
}

TEST_CASE("MqttNamesAreEncodedInTopics") {
	Fixture f;
	f.load(kinds());
	f.connect();
	f.client.published.clear();
	Sensor bus;
	f.bind(bus, "sensor/odd+name#1");
	f.report(bus, "sensor/odd+name#1", Value::of_real(7));
	f.settle();
	CHECK(f.client.to("house/main/dp/sensor%2Fodd%2Bname%231/state").size() == 1);
	// A command to an encoded name reaches the datapoint (it is not writable: refused by name).
	f.client.deliver("house/main/dp/sensor%2Fodd%2Bname%231/set", "1");
	auto e = f.client.to("house/main/dp/sensor%2Fodd%2Bname%231/error");
	REQUIRE(e.size() == 1);
	CHECK(e[0].payload.find("not writable") != std::string::npos);
	f.client.deliver("house/main/dp/bad%2/set", "1");
	CHECK(f.capture.sink.contains("the name is malformed"));
}

TEST_CASE("MqttDiscoveryOfTheExampleImage") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	check_snapshot(discovery_lines(f.client, "homeassistant"), "fixtures/mqtt/example.discovery.txt");
}

TEST_CASE("MqttDiscoveryOfEveryKind") {
	Fixture f;
	f.load(kinds());
	f.connect();
	std::string lines = discovery_lines(f.client, "ha");
	check_snapshot(lines, "fixtures/mqtt/kinds.discovery.txt");
	// Every payload is JSON.
	std::istringstream in(lines);
	std::string line;
	int count = 0;
	while (std::getline(in, line)) {
		size_t space = line.find(' ');
		hc::JsonSpan span;
		CHECK(hc::json_parse(line.data() + space + 1, line.size() - space - 1, span));
		++count;
	}
	CHECK(count == 24);
}

TEST_CASE("MqttDiscoveryOff") {
	Fixture f;
	f.load(example(), "{\"broker\": \"mqtt://broker.example\", \"discovery\": {\"enabled\": false}}");
	f.connect();
	CHECK(f.client.under("homeassistant/").empty());
	const char* error = nullptr;
	CHECK_FALSE(f.mqtt->publish_discovery(&error));
	CHECK(std::string(error).find("discovery is off") != std::string::npos);
}

TEST_CASE("MqttSweepRemovesVanishedFunctionsAndDatapoints") {
	Fixture f;
	// What an earlier image left on the broker.
	f.client.retained["homeassistant/light/homecontrol-a1b2c3/old-light/config"] = "{\"name\":\"old\"}";
	f.client.retained["homeassistant/sensor/homecontrol-a1b2c3/hall-light/config"] = "{\"name\":\"kind changed\"}";
	f.client.retained["homeassistant/light/homecontrol-other/old-light/config"] = "{\"name\":\"another node\"}";
	f.client.retained["homecontrol/a1b2c3/dp/gone.value/state"] = "{\"value\":1,\"quality\":\"good\"}";
	f.load(example(), kMember);
	f.connect();
	f.client.deliver_retained();
	f.settle();
	CHECK(f.client.retained.count("homeassistant/light/homecontrol-a1b2c3/old-light/config") == 0);
	CHECK(f.client.retained.count("homeassistant/sensor/homecontrol-a1b2c3/hall-light/config") == 0);
	CHECK(f.client.retained.count("homeassistant/light/homecontrol-a1b2c3/hall-light/config") == 1);
	CHECK(f.client.retained.count("homeassistant/light/homecontrol-other/old-light/config") == 1);
	CHECK(f.client.retained.count("homecontrol/a1b2c3/dp/gone.value/state") == 0);
	CHECK(f.capture.sink.contains("removed the retained homeassistant/light/homecontrol-a1b2c3/old-light/config"));
	// Live (not retained) messages and messages after the sweep change nothing.
	f.client.deliver("homeassistant/switch/homecontrol-a1b2c3/x/config", "{}", false);
	CHECK(f.client.to("homeassistant/switch/homecontrol-a1b2c3/x/config").empty());
	f.run_for(Mqtt::kSweepMs);
	CHECK_FALSE(f.client.subscribed("homeassistant/+/homecontrol-a1b2c3/+/config"));
	CHECK_FALSE(f.client.subscribed("homecontrol/a1b2c3/dp/+/state"));
	CHECK(f.client.subscribed("homecontrol/a1b2c3/dp/+/set"));
	f.client.deliver("homeassistant/switch/homecontrol-a1b2c3/y/config", "{}", true);
	CHECK(f.client.to("homeassistant/switch/homecontrol-a1b2c3/y/config").empty());
}

TEST_CASE("MqttSweepRetriesWhenTheQueueIsFull") {
	Fixture f;
	f.client.retained["homeassistant/light/homecontrol-a1b2c3/old-light/config"] = "{\"name\":\"old\"}";
	f.load(example(), kMember);
	f.connect();
	f.client.capacity = f.client.queued;
	f.client.deliver_retained();
	CHECK(f.client.retained.count("homeassistant/light/homecontrol-a1b2c3/old-light/config") == 1);
	f.client.queued = 0;
	f.client.capacity = SIZE_MAX;
	f.run_for(Mqtt::kSweepMs); // the sweep ends and starts again
	CHECK(f.client.subscribed("homeassistant/+/homecontrol-a1b2c3/+/config"));
	f.client.deliver_retained();
	CHECK(f.client.retained.count("homeassistant/light/homecontrol-a1b2c3/old-light/config") == 0);
}

TEST_CASE("MqttImageSwapRepublishesAndRemoves") {
	Fixture f;
	f.load(kinds());
	f.connect();
	f.run_for(Mqtt::kSweepMs);
	CHECK(f.client.retained.count("ha/light/homecontrol-a1b2c3/dimmer/config") == 1);
	f.client.published.clear();
	// The same connection, fewer functions: the connection stays, the vanished configs go.
	std::string text = kinds();
	size_t at = text.find("\"id\": \"dimmer\"");
	REQUIRE(at != std::string::npos);
	text.replace(at, 14, "\"id\": \"dimmer2\"");
	f.load(text);
	f.settle();
	CHECK(f.client.connects == 1);
	CHECK(f.client.disconnects == 0);
	CHECK(f.client.to("ha/light/homecontrol-a1b2c3/dimmer2/config").size() == 1);
	f.client.deliver_retained();
	f.settle();
	CHECK(f.client.retained.count("ha/light/homecontrol-a1b2c3/dimmer/config") == 0);
	CHECK(f.client.retained.count("ha/light/homecontrol-a1b2c3/dimmer2/config") == 1);
	CHECK(f.client.under("ha/").size() == 25); // 24 configs again, one removal
}

TEST_CASE("MqttNewBaseTopicRetiresAndReconnects") {
	Fixture f;
	f.load(kinds());
	f.connect();
	f.client.published.clear();
	std::string text = kinds();
	size_t at = text.find("house/main");
	text.replace(at, 10, "house/next");
	f.load(text);
	CHECK(f.mqtt->state() == MqttState::retiring);
	f.settle();
	auto offline = f.client.to("house/main/status");
	REQUIRE(offline.size() == 1);
	CHECK(offline[0].payload == "offline");
	CHECK(f.client.under("ha/").empty()); // same broker and prefix: the configs stay
	f.client.queued = 0; // drained
	f.run_for(Mqtt::kRetirePollMs);
	CHECK(f.client.disconnects == 1);
	CHECK(f.client.connects == 2);
	CHECK(f.client.will_topic == "house/next/status");
	f.client.accept();
	f.settle();
	CHECK(f.client.to("house/next/status").back().payload == "online");
}

TEST_CASE("MqttClearRemovesTheConfigsAndGoesOffline") {
	Fixture f;
	f.load(example(), kMember);
	f.connect();
	f.client.published.clear();
	std::string text = example(); // no mqtt member
	f.load_model(text);
	REQUIRE(f.mqtt->configure_image(text.data(), text.size()));
	f.settle();
	CHECK(f.client.under("homeassistant/").size() == 9);
	for (const auto& p : f.client.under("homeassistant/")) {
		CHECK(p.payload.empty());
		CHECK(p.retain);
	}
	CHECK(f.client.to("homecontrol/a1b2c3/status").back().payload == "offline");
	f.run_for(Mqtt::kRetireMs); // the queue never drains: the deadline ends it
	CHECK(f.client.disconnects == 1);
	CHECK(f.mqtt->state() == MqttState::disabled);
	f.run_for(600000);
	CHECK(f.client.connects == 1);
}

TEST_CASE("MqttDisabledByTheImage") {
	Fixture f;
	f.load(example(), "{\"broker\": \"mqtt://broker.example\", \"enabled\": false}");
	f.run_for(60000);
	CHECK(f.client.connects == 0);
	CHECK(f.mqtt->state() == MqttState::disabled);
}

TEST_CASE("MqttMemberRefusals") {
	struct Case {
		const char* member;
		const char* error;
	};
	const Case cases[] = {
		{"[]", "mqtt: must be an object"},
		{"{}", "mqtt.broker: is missing"},
		{"{\"broker\": \"tcp://x\"}", "the broker URL must start with mqtt:// or mqtts://"},
		{"{\"broker\": \"mqtt://\"}", "the broker URL has no host"},
		{"{\"broker\": \"mqtt://h:0\"}", "the broker port must be 1..65535"},
		{"{\"broker\": \"mqtt://h/path\"}", "no path"},
		{"{\"broker\": \"mqtt://h\", \"password\": \"pw\"}", "the device setting mqtt.password"},
		{"{\"broker\": \"mqtt://h\", \"baseTopic\": \"a/+/b\"}", "mqtt.baseTopic"},
		{"{\"broker\": \"mqtt://h\", \"baseTopic\": \"/a\"}", "mqtt.baseTopic"},
		{"{\"broker\": \"mqtt://h\", \"baseTopic\": \"a//b\"}", "mqtt.baseTopic"},
		{"{\"broker\": \"mqtt://h\", \"discovery\": {\"prefix\": \"ha#\"}}", "mqtt.discovery.prefix"},
		{"{\"broker\": \"mqtt://h\", \"history\": {\"interval\": 0}}", "mqtt.history.interval"},
		{"{\"broker\": \"mqtt://h\", \"history\": {\"interval\": 2.5}}", "mqtt.history.interval"},
		{"{\"broker\": \"mqtt://h\", \"retain\": 1}", "mqtt.retain"},
		{"{\"broker\": \"mqtt://h\", \"username\": \"\"}", "mqtt.username"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.member);
		const char* error = nullptr;
		CHECK_FALSE(Mqtt::check_json(c.member, std::strlen(c.member), &error));
		REQUIRE(error != nullptr);
		CHECK(std::string(error).find(c.error) != std::string::npos);
	}
	const char* ok = "{\"broker\": \"mqtts://broker.example:8883/\", \"username\": \"u\", \"comment\": \"x\"}";
	CHECK(Mqtt::check_json(ok, std::strlen(ok), nullptr));
	// A refused configuration keeps the previous one.
	Fixture f;
	f.load(example(), kMember);
	const char* bad = "{\"broker\": \"mqtt://h\", \"password\": \"pw\"}";
	const char* error = nullptr;
	CHECK_FALSE(f.mqtt->configure_json(bad, std::strlen(bad), &error));
	CHECK(std::string(f.mqtt->base_topic()) == "homecontrol/a1b2c3");
	CHECK(f.capture.sink.contains("configuration refused: mqtt.password"));
}

TEST_CASE("MqttHistoryExport") {
	Fixture f;
	f.connectivity.link_down("test"); // collect samples before the first connection
	f.load(kinds());
	Sensor bus;
	f.bind(bus, "sensor.temperature");
	for (int i = 0; i < 10; ++i) {
		f.report(bus, "sensor.temperature", Value::of_real(20 + i * 0.25));
		f.run_for(60000);
	}
	f.connectivity.link_up("192.0.2.100");
	f.run_for(Mqtt::kReadyPollMs);
	f.sync_wall();
	int64_t export_wall = f.clock.wall_seconds;
	f.connect();
	// Exactly the ring's samples before the second of the export, value for value.
	uint16_t id = f.dp("sensor.temperature").id();
	hc::HistoryPoint ring[64];
	size_t count = 0;
	REQUIRE(f.history.range(id, 0, export_wall, ring, 64, count) == hc::HistoryStatus::ok);
	REQUIRE(count >= 10);
	REQUIRE(count <= Mqtt::kHistoryBatch);
	std::string expected = "[";
	for (size_t i = 0; i < count; ++i) {
		char text[64];
		std::snprintf(text, sizeof(text), "%s[%lld,%g]", i > 0 ? "," : "", static_cast<long long>(ring[i].t), ring[i].last);
		expected += text;
	}
	expected += "]";
	auto h = f.client.to("house/main/history/sensor.temperature");
	REQUIRE(h.size() == 1);
	CHECK(h[0].payload == expected);
	CHECK_FALSE(h[0].retain);
	CHECK(h[0].qos == 0);
	CHECK(f.client.under("house/main/history/").size() == 1); // no samples, no message

	// The next export (interval 5 min) carries only what is new.
	f.report(bus, "sensor.temperature", Value::of_real(30));
	f.run_for(5 * 60000);
	h = f.client.to("house/main/history/sensor.temperature");
	REQUIRE(h.size() == 2);
	CHECK(h[1].payload.find(",30]") != std::string::npos);

	// An image swap keeps the cursor (the ring follows the name), and so does a reconnect.
	f.load(kinds());
	f.settle();
	f.client.drop("test");
	f.run_for(Mqtt::kRetryInitialMs);
	f.client.accept();
	f.settle();
	f.run_for(5 * 60000);
	// Across all messages every sample went out once, in order.
	int64_t previous = 0;
	for (const auto& p : f.client.to("house/main/history/sensor.temperature")) {
		hc::JsonSpan array;
		REQUIRE(hc::json_parse(p.payload.data(), p.payload.size(), array));
		size_t position = 0;
		hc::JsonSpan item;
		while (hc::json_next_item(array, position, item)) {
			size_t inner = 0;
			hc::JsonSpan t;
			REQUIRE(hc::json_next_item(item, inner, t));
			double time = 0;
			REQUIRE(hc::json_number(t, time));
			CHECK(static_cast<int64_t>(time) > previous);
			previous = static_cast<int64_t>(time);
		}
	}
	MqttStatus status;
	f.mqtt->status(status);
	CHECK(status.history);
	CHECK(status.history_interval_min == 5);
	CHECK(status.history_messages >= 3);
}

TEST_CASE("MqttHistoryExportIsBatched") {
	Fixture f;
	f.connectivity.link_down("test");
	f.load(kinds());
	Sensor bus;
	f.bind(bus, "sensor.window"); // spacing 10 s
	for (int i = 0; i < 100; ++i) {
		f.report(bus, "sensor.window", Value::of_bool(i % 2 == 0));
		f.run_for(10000);
	}
	f.connectivity.link_up("192.0.2.100");
	f.run_for(Mqtt::kReadyPollMs);
	f.connect();
	auto h = f.client.to("house/main/history/sensor.window");
	REQUIRE(h.size() >= 4);
	int64_t previous = 0;
	size_t total = 0;
	for (const auto& p : h) {
		hc::JsonSpan array;
		REQUIRE(hc::json_parse(p.payload.data(), p.payload.size(), array));
		size_t position = 0;
		hc::JsonSpan item;
		size_t n = 0;
		while (hc::json_next_item(array, position, item)) {
			size_t inner = 0;
			hc::JsonSpan t, v;
			REQUIRE(hc::json_next_item(item, inner, t));
			REQUIRE(hc::json_next_item(item, inner, v));
			CHECK(v.kind == hc::JsonSpan::Kind::boolean);
			double time = 0;
			REQUIRE(hc::json_number(t, time));
			CHECK(static_cast<int64_t>(time) > previous); // ascending, never twice
			previous = static_cast<int64_t>(time);
			++n;
		}
		CHECK(n <= Mqtt::kHistoryBatch);
		total += n;
	}
	CHECK(total == f.history.samples(f.dp("sensor.window").id()));
}

TEST_CASE("MqttHistoryExportWaitsForTheWallClock") {
	Fixture f;
	f.connectivity.link_down("test");
	f.load(kinds());
	Sensor bus;
	f.bind(bus, "sensor.temperature");
	f.report(bus, "sensor.temperature", Value::of_real(20));
	f.run_for(60000);
	REQUIRE(f.history.samples(f.dp("sensor.temperature").id()) > 0);
	f.clock.synchronized = false;
	f.connectivity.link_up("192.0.2.100");
	f.run_for(Mqtt::kReadyPollMs);
	f.client.accept();
	f.loop.run_once(f.clock.now_ms);
	f.loop.run_once(f.clock.now_ms);
	CHECK(f.client.under("house/main/history/").empty());
	CHECK(f.capture.sink.contains("history export waits for the wall clock"));
}

} // TEST_SUITE
