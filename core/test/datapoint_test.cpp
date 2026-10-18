/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/event_loop.h"
#include "hc/technology.h"
#include "test_support.h"

using hc::Datapoint;
using hc::DatapointFlags;
using hc::DatapointListener;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::DatapointUpdate;
using hc::Quality;
using hc::UpdateOrigin;
using hc::Value;
using hc::ValueType;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kRW = DatapointFlags::kReadable | DatapointFlags::kWritable;
constexpr uint8_t kRWT = kRW | DatapointFlags::kTransmit;

/// A technology whose bus is a loop: every command it accepts is recorded and, with `echo`, comes
/// back as a report on the next run of the event loop (as a KNX bus mirrors a telegram to the other
/// devices listening on the group address).
class LoopbackTechnology : public hc::Technology {
public:
	LoopbackTechnology(DatapointModel& model, hc::EventLoop& loop) : model_(model), loop_(loop) {}

	const char* name() const override { return "loopback"; }

	bool command(const Datapoint& datapoint, const Value& value) override {
		commands.push_back({datapoint.id(), value});
		// The model refuses re-entry while the technology handles a command.
		reentry = model_.report(*this, datapoint.id(), value);
		if (!accept) {
			return false;
		}
		if (echo && pending_count_ < kPending) {
			pending_[pending_count_++] = {datapoint.id(), value};
			loop_.post(hc::Callback::member<LoopbackTechnology, &LoopbackTechnology::deliver>(this));
		}
		return true;
	}

	/// The bus says the value of `id` is `value`.
	DatapointStatus receive(uint16_t id, const Value& value) { return model_.report(*this, id, value); }

	struct Command {
		uint16_t id;
		Value value;
	};

	std::vector<Command> commands;
	bool accept = true;
	bool echo = false;
	DatapointStatus reentry = DatapointStatus::ok;

private:
	void deliver() {
		for (size_t i = 0; i < pending_count_; ++i) {
			model_.report(*this, pending_[i].id, pending_[i].value);
		}
		pending_count_ = 0;
	}

	static constexpr size_t kPending = 8;
	DatapointModel& model_;
	hc::EventLoop& loop_;
	Command pending_[kPending];
	size_t pending_count_ = 0;
};

/// Records every update; runs an optional action inside the notification.
class RecordingListener : public DatapointListener {
public:
	explicit RecordingListener(std::string tag_, std::vector<std::string>* order_ = nullptr)
		: tag(std::move(tag_)), order(order_) {}

	void datapoint_updated(const DatapointUpdate& update) override {
		char old_text[48];
		char new_text[48];
		std::string line = std::string(update.datapoint.name()) + " " + update.old_value.format(old_text, sizeof(old_text)) +
			" -> " + update.datapoint.value().format(new_text, sizeof(new_text)) + " (" +
			hc::update_origin_name(update.origin) + ", " + hc::quality_name(update.old_quality) + " -> " +
			hc::quality_name(update.datapoint.quality()) + (update.changed() ? ", changed" : "") + ")";
		lines.push_back(line);
		if (order != nullptr) {
			order->push_back(tag);
		}
		if (action) {
			action(update);
		}
	}

	std::string tag;
	std::vector<std::string>* order;
	std::vector<std::string> lines;
	std::function<void(const DatapointUpdate&)> action;
};

struct Fixture {
	ScopedCapture capture;
	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model{clock};
	LoopbackTechnology knx{model, loop};

	Fixture() {
		const DatapointSpec specs[] = {
			{"Kitchen.Light", ValueType::boolean, kRWT},
			{"Kitchen.Temperature", ValueType::real, DatapointFlags::kReadable | DatapointFlags::kReadOnStart},
			{"Heating.Setpoint", ValueType::real, kRWT},
			{"Rule.Enabled", ValueType::boolean, kRW},
			{"Status.Text", ValueType::string, kRW},
		};
		REQUIRE(model.load(specs, sizeof(specs) / sizeof(specs[0])) == DatapointStatus::ok);
		for (uint16_t id = 0; id < 3; ++id) {
			REQUIRE(model.bind(id, knx) == DatapointStatus::ok);
		}
	}
};

} // namespace

TEST_CASE("Datapoint values are 32-byte typed values") {
	CHECK(sizeof(Value) == 32);
	Value none;
	CHECK(none.empty());
	CHECK(Value::of_real(1.5).as_real() == 1.5);
	CHECK(Value::of_real(1.5).as_unsigned() == 0); // another type reads as zero
	CHECK(Value::of_real(std::nan("")).empty());
	CHECK(Value::of_bool(true) == Value::of_bool(true));
	CHECK(Value::of_bool(true) != Value::of_unsigned(1));
	CHECK(Value::of_unsigned(7) != Value::of_unsigned(8));

	std::optional<Value> text = Value::of_string("0123456789012345678901234567"); // 28 bytes
	REQUIRE(text.has_value());
	CHECK(std::string(text->as_string()) == "0123456789012345678901234567");
	CHECK(text->length() == 28);
	CHECK(!Value::of_string("01234567890123456789012345678").has_value()); // 29 bytes: refused, not cut
	CHECK(!Value::of_string("a\0b", 3).has_value());
	CHECK(std::string(Value::of_bool(true).as_string()).empty());

	uint8_t bytes[15] = {1, 2, 3};
	std::optional<Value> raw = Value::of_raw(bytes, 3);
	REQUIRE(raw.has_value());
	CHECK(raw->raw()[2] == 3);
	CHECK(raw->length() == 3);
	CHECK(!Value::of_raw(bytes, 15).has_value());
	CHECK(*raw != *Value::of_raw(bytes, 2));

	char line[64];
	CHECK(std::string(Value::of_time(hc::TimeOfDay{2, 13, 45, 30}).format(line, sizeof(line))) == "13:45:30 Tue");
	CHECK(std::string(raw->format(line, sizeof(line))) == "01 02 03");
	CHECK(std::string(Value::of_signed(-3).format(line, sizeof(line))) == "-3");
	CHECK(std::string(text->format(line, 8)) == "\"012345");
}

TEST_CASE("Datapoint value conversion is exact or refused") {
	CHECK(*Value::of_unsigned(5).convert(ValueType::real) == Value::of_real(5));
	CHECK(*Value::of_real(5).convert(ValueType::unsigned_int) == Value::of_unsigned(5));
	CHECK(*Value::of_signed(-5).convert(ValueType::signed_int) == Value::of_signed(-5));
	CHECK(*Value::of_unsigned(3).convert(ValueType::enumeration) == Value::of_enum(3));
	CHECK(!Value::of_real(5.5).convert(ValueType::unsigned_int).has_value());
	CHECK(!Value::of_signed(-1).convert(ValueType::unsigned_int).has_value());
	CHECK(!Value::of_unsigned(3000000000u).convert(ValueType::signed_int).has_value());
	CHECK(!Value::of_bool(true).convert(ValueType::unsigned_int).has_value());
	CHECK(!Value::of_unsigned(1).convert(ValueType::boolean).has_value());
	CHECK(!Value().convert(ValueType::real).has_value());
	CHECK(Value().convert(ValueType::none).has_value());
}

TEST_CASE("DatapointModel load builds the table with stable ids and a name index") {
	Fixture f;
	CHECK(f.model.size() == 5);
	const Datapoint* light = f.model.get(0);
	REQUIRE(light != nullptr);
	CHECK(light->id() == 0);
	CHECK(std::string(light->name()) == "Kitchen.Light");
	CHECK(light->type() == ValueType::boolean);
	CHECK(light->writable());
	CHECK(light->transmits());
	CHECK(light->readable());
	CHECK(!light->read_on_start());
	CHECK(light->quality() == Quality::unknown);
	CHECK(light->value().empty());
	CHECK(light->updated_ms() == 0);
	CHECK(light->technology() == &f.knx);
	CHECK(f.model.get(3)->technology() == nullptr);
	CHECK(f.model.get(1)->read_on_start());
	CHECK(f.model.get(5) == nullptr);
	CHECK(f.model.find("Status.Text") == f.model.get(4));
	CHECK(f.model.find("Heating.Setpoint") == f.model.get(2));
	CHECK(f.model.find("Kitchen") == nullptr);
	CHECK(f.model.find("Zzz") == nullptr);
	CHECK(f.model.find(nullptr) == nullptr);
	CHECK(f.capture.sink.contains("model loaded: 5 datapoints"));
}

TEST_CASE("DatapointModel load validates every spec before it replaces the model") {
	Fixture f;
	std::string long_name(64, 'x');
	const DatapointSpec duplicate[] = {{"A", ValueType::boolean, 0}, {"B", ValueType::real, 0}, {"A", ValueType::real, 0}};
	const DatapointSpec too_long[] = {{long_name.c_str(), ValueType::boolean, 0}};
	const DatapointSpec empty_name[] = {{"", ValueType::boolean, 0}};
	const DatapointSpec null_name[] = {{nullptr, ValueType::boolean, 0}};
	const DatapointSpec control[] = {{"Tab\there", ValueType::boolean, 0}};
	const DatapointSpec no_type[] = {{"A", ValueType::none, 0}};
	const DatapointSpec flags[] = {{"A", ValueType::boolean, 0x40}};
	CHECK(f.model.load(duplicate, 3) == DatapointStatus::invalid_model);
	CHECK(f.capture.sink.contains("datapoints 0 and 2 are both named 'A'"));
	CHECK(f.model.load(too_long, 1) == DatapointStatus::invalid_model);
	CHECK(f.capture.sink.contains("too long"));
	CHECK(f.model.load(empty_name, 1) == DatapointStatus::invalid_model);
	CHECK(f.model.load(null_name, 1) == DatapointStatus::invalid_model);
	CHECK(f.model.load(control, 1) == DatapointStatus::invalid_model);
	CHECK(f.model.load(no_type, 1) == DatapointStatus::invalid_model);
	CHECK(f.model.load(flags, 1) == DatapointStatus::invalid_model);
	CHECK(f.model.load(duplicate, DatapointModel::kMaxDatapoints + 1) == DatapointStatus::capacity);
	CHECK(f.model.load(nullptr, 3) == DatapointStatus::invalid_model);
	// The previous model is untouched.
	CHECK(f.model.size() == 5);
	CHECK(f.model.find("Kitchen.Light") == f.model.get(0));

	std::string max_name(63, 'y');
	const DatapointSpec replacement[] = {{max_name.c_str(), ValueType::unsigned_int, kRW}};
	CHECK(f.model.load(replacement, 1) == DatapointStatus::ok);
	CHECK(f.model.size() == 1);
	CHECK(f.model.get(0)->technology() == nullptr); // bindings belong to the old model
	CHECK(f.model.find(max_name.c_str()) == f.model.get(0));
	CHECK(f.model.load(nullptr, 0) == DatapointStatus::ok);
	CHECK(f.model.size() == 0);
	CHECK(f.model.find("x") == nullptr);
}

TEST_CASE("DatapointModel loads a large model and finds every name") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model{clock};
	std::vector<std::string> names;
	for (int i = 0; i < 2000; ++i) {
		names.push_back("Group." + std::to_string((i * 7919) % 2000));
	}
	std::vector<DatapointSpec> specs;
	for (const std::string& name : names) {
		specs.push_back({name.c_str(), ValueType::real, 0});
	}
	REQUIRE(model.load(specs.data(), specs.size()) == DatapointStatus::ok);
	for (size_t i = 0; i < names.size(); ++i) {
		const Datapoint* dp = model.find(names[i].c_str());
		REQUIRE(dp != nullptr);
		CHECK(dp->id() == i);
	}
	model.clear();
	CHECK(model.size() == 0);
}

TEST_CASE("DatapointModel report stores the value, timestamp and quality and tells listeners") {
	Fixture f;
	RecordingListener listener("a");
	REQUIRE(f.model.add_listener(listener) == DatapointStatus::ok);
	f.clock.now_ms = 5000;
	CHECK(f.knx.receive(1, Value::of_real(21.5)) == DatapointStatus::ok);
	const Datapoint* temperature = f.model.get(1);
	CHECK(temperature->value() == Value::of_real(21.5));
	CHECK(temperature->quality() == Quality::good);
	CHECK(temperature->updated_ms() == 5000);
	REQUIRE(listener.lines.size() == 1);
	CHECK(listener.lines[0] == "Kitchen.Temperature (none) -> 21.5 (report, unknown -> good, changed)");

	// The same value again is an update (repeated telegrams matter), but not a change.
	f.clock.now_ms = 6000;
	CHECK(f.knx.receive(1, Value::of_real(21.5)) == DatapointStatus::ok);
	CHECK(temperature->updated_ms() == 6000);
	REQUIRE(listener.lines.size() == 2);
	CHECK(listener.lines[1] == "Kitchen.Temperature 21.5 -> 21.5 (report, good -> good)");

	// An exact conversion is accepted, an inexact one refused with a log line.
	CHECK(f.knx.receive(1, Value::of_signed(22)) == DatapointStatus::ok);
	CHECK(temperature->value() == Value::of_real(22));
	CHECK(f.knx.receive(0, Value::of_real(1)) == DatapointStatus::wrong_type);
	CHECK(f.capture.sink.contains("refusing report of 'Kitchen.Light' from loopback: a real value for a boolean datapoint"));
	CHECK(f.knx.receive(9, Value::of_bool(true)) == DatapointStatus::unknown_id);
	CHECK(f.capture.sink.contains("no datapoint with id 9"));
	CHECK(listener.lines.size() == 3);
}

TEST_CASE("DatapointModel report from a technology that does not own the datapoint is refused") {
	Fixture f;
	LoopbackTechnology zigbee{f.model, f.loop};
	CHECK(zigbee.receive(0, Value::of_bool(true)) == DatapointStatus::not_owner);
	CHECK(zigbee.receive(3, Value::of_bool(true)) == DatapointStatus::not_owner); // application datapoint
	CHECK(f.model.get(0)->quality() == Quality::unknown);
	CHECK(f.model.bind(0, zigbee) == DatapointStatus::not_owner);
	CHECK(f.capture.sink.contains("refusing to bind 'Kitchen.Light' to loopback: owned by loopback"));
	CHECK(f.model.bind(0, f.knx) == DatapointStatus::ok); // binding the owner again is harmless
	CHECK(f.model.bind(77, zigbee) == DatapointStatus::unknown_id);
}

TEST_CASE("DatapointModel command goes to the owning technology, then into the model") {
	Fixture f;
	RecordingListener listener("a");
	f.model.add_listener(listener);
	f.clock.now_ms = 7000;
	CHECK(f.model.command(2, Value::of_real(21.0)) == DatapointStatus::ok);
	REQUIRE(f.knx.commands.size() == 1);
	CHECK(f.knx.commands[0].id == 2);
	CHECK(f.knx.commands[0].value == Value::of_real(21.0));
	const Datapoint* setpoint = f.model.get(2);
	CHECK(setpoint->value() == Value::of_real(21.0));
	CHECK(setpoint->quality() == Quality::good);
	CHECK(setpoint->updated_ms() == 7000);
	REQUIRE(listener.lines.size() == 1);
	CHECK(listener.lines[0] == "Heating.Setpoint (none) -> 21 (command, unknown -> good, changed)");

	// The technology must not call back into the model while it handles the command.
	CHECK(f.knx.reentry == DatapointStatus::busy);
	CHECK(f.capture.sink.contains("refusing report from within a technology's command"));

	// The value is converted before the technology sees it.
	CHECK(f.model.command(2, Value::of_unsigned(19)) == DatapointStatus::ok);
	CHECK(f.knx.commands.back().value == Value::of_real(19));
}

TEST_CASE("DatapointModel command refusals leave the value alone") {
	Fixture f;
	RecordingListener listener("a");
	f.model.add_listener(listener);
	CHECK(f.model.command(1, Value::of_real(20)) == DatapointStatus::not_writable);
	CHECK(f.capture.sink.contains("refusing command to 'Kitchen.Temperature': not writable"));
	CHECK(f.model.command(0, Value::of_real(1)) == DatapointStatus::wrong_type);
	CHECK(f.capture.sink.contains("refusing command to 'Kitchen.Light': a real value for a boolean datapoint"));
	CHECK(f.model.command(42, Value::of_bool(true)) == DatapointStatus::unknown_id);
	f.knx.accept = false;
	CHECK(f.model.command(0, Value::of_bool(true)) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("command on to 'Kitchen.Light' refused by loopback"));
	CHECK(f.knx.commands.size() == 1);
	CHECK(f.model.get(0)->quality() == Quality::unknown);
	CHECK(listener.lines.empty());
}

TEST_CASE("DatapointModel command to a datapoint without transmit or technology changes only the model") {
	Fixture f;
	CHECK(f.model.command(3, Value::of_bool(true)) == DatapointStatus::ok); // no technology
	CHECK(f.model.get(3)->value() == Value::of_bool(true));
	CHECK(f.model.command(4, *Value::of_string("Away")) == DatapointStatus::ok);
	CHECK(std::string(f.model.get(4)->value().as_string()) == "Away");

	const DatapointSpec specs[] = {{"Status", ValueType::unsigned_int, kRW}}; // writable, no transmit
	REQUIRE(f.model.load(specs, 1) == DatapointStatus::ok);
	REQUIRE(f.model.bind(0, f.knx) == DatapointStatus::ok);
	CHECK(f.model.command(0, Value::of_unsigned(3)) == DatapointStatus::ok);
	CHECK(f.knx.commands.empty());
	CHECK(f.model.get(0)->value() == Value::of_unsigned(3));
	// The technology still reports it.
	CHECK(f.knx.receive(0, Value::of_unsigned(4)) == DatapointStatus::ok);
	CHECK(f.model.get(0)->value() == Value::of_unsigned(4));
}

TEST_CASE("DatapointModel loopback: a command comes back as a report on the next loop run") {
	Fixture f;
	RecordingListener listener("a");
	f.model.add_listener(listener);
	f.knx.echo = true;
	CHECK(f.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(listener.lines.size() == 1);
	f.loop.run_once(f.clock.now_ms);
	REQUIRE(listener.lines.size() == 2);
	CHECK(listener.lines[1] == "Kitchen.Light on -> on (report, good -> good)");
}

TEST_CASE("DatapointModel listeners run in registration order with old and new value") {
	Fixture f;
	std::vector<std::string> order;
	RecordingListener a("a", &order);
	RecordingListener b("b", &order);
	RecordingListener c("c", &order);
	REQUIRE(f.model.add_listener(a) == DatapointStatus::ok);
	REQUIRE(f.model.add_listener(b) == DatapointStatus::ok);
	REQUIRE(f.model.add_listener(c) == DatapointStatus::ok);
	CHECK(f.model.add_listener(b) == DatapointStatus::capacity); // duplicate
	CHECK(f.capture.sink.contains("already registered"));
	f.knx.receive(0, Value::of_bool(false));
	f.knx.receive(0, Value::of_bool(true));
	CHECK(order == std::vector<std::string>{"a", "b", "c", "a", "b", "c"});
	CHECK(c.lines[1] == "Kitchen.Light off -> on (report, good -> good, changed)");
	CHECK(f.model.remove_listener(b));
	CHECK(!f.model.remove_listener(b));
	order.clear();
	f.knx.receive(0, Value::of_bool(false));
	CHECK(order == std::vector<std::string>{"a", "c"});
}

TEST_CASE("DatapointModel listener capacity is enforced") {
	Fixture f;
	std::vector<RecordingListener> listeners;
	listeners.reserve(DatapointModel::kListenerCapacity + 1);
	for (size_t i = 0; i <= DatapointModel::kListenerCapacity; ++i) {
		listeners.emplace_back(std::to_string(i));
	}
	for (size_t i = 0; i < DatapointModel::kListenerCapacity; ++i) {
		CHECK(f.model.add_listener(listeners[i]) == DatapointStatus::ok);
	}
	CHECK(f.model.add_listener(listeners.back()) == DatapointStatus::capacity);
	CHECK(f.capture.sink.contains("datapoint listeners exhausted (capacity 8)"));
}

TEST_CASE("DatapointModel listeners must not change the model synchronously") {
	Fixture f;
	RecordingListener rule("rule");
	RecordingListener after("after");
	std::vector<DatapointStatus> attempts;
	rule.action = [&](const DatapointUpdate& update) {
		if (update.datapoint.id() != 0) {
			return;
		}
		attempts.push_back(f.model.command(3, Value::of_bool(true)));
		attempts.push_back(f.knx.receive(1, Value::of_real(5)));
		attempts.push_back(f.model.report_stale(f.knx, 1));
		attempts.push_back(f.model.add_listener(after));
		CHECK(!f.model.remove_listener(rule));
		const DatapointSpec specs[] = {{"X", ValueType::boolean, 0}};
		attempts.push_back(f.model.load(specs, 1));
		attempts.push_back(f.model.bind(3, f.knx));
		CHECK(f.model.notifying());
		// Reading is fine.
		CHECK(f.model.get(0)->value() == Value::of_bool(true));
	};
	f.model.add_listener(rule);
	CHECK(f.knx.receive(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(attempts == std::vector<DatapointStatus>(6, DatapointStatus::busy));
	CHECK(f.capture.sink.count("from a datapoint listener (post it to the event loop)") == 7);
	CHECK(f.model.get(3)->quality() == Quality::unknown);
	CHECK(f.model.size() == 5);
	CHECK(!f.model.notifying());
}

namespace {

/// A rule: when the light goes on, it switches the rule's enable flag - through the event loop.
struct PostingRule : DatapointListener {
	PostingRule(DatapointModel& model_, hc::EventLoop& loop_) : model(model_), loop(loop_) {}

	void datapoint_updated(const DatapointUpdate& update) override {
		if (update.datapoint.id() == 0 && update.changed()) {
			loop.post(hc::Callback::member<PostingRule, &PostingRule::act>(this));
		}
	}

	void act() { result = model.command(3, Value::of_bool(true)); }

	DatapointModel& model;
	hc::EventLoop& loop;
	DatapointStatus result = DatapointStatus::busy;
};

} // namespace

TEST_CASE("DatapointModel a listener reacts with a change through the event loop") {
	Fixture f;
	PostingRule rule{f.model, f.loop};
	f.model.add_listener(rule);
	f.knx.receive(0, Value::of_bool(true));
	CHECK(f.model.get(3)->quality() == Quality::unknown);
	f.loop.run_once(f.clock.now_ms);
	CHECK(rule.result == DatapointStatus::ok);
	CHECK(f.model.get(3)->value() == Value::of_bool(true));
}

TEST_CASE("DatapointModel stale reports keep the value and tell listeners once") {
	Fixture f;
	RecordingListener listener("a");
	f.model.add_listener(listener);
	// Unknown stays unknown.
	CHECK(f.model.report_stale(f.knx, 1) == DatapointStatus::ok);
	CHECK(f.model.get(1)->quality() == Quality::unknown);
	CHECK(listener.lines.empty());
	f.clock.now_ms = 3000;
	f.knx.receive(1, Value::of_real(20.5));
	f.clock.now_ms = 9000;
	CHECK(f.model.report_stale(f.knx, 1) == DatapointStatus::ok);
	CHECK(f.model.get(1)->quality() == Quality::stale);
	CHECK(f.model.get(1)->value() == Value::of_real(20.5));
	CHECK(f.model.get(1)->updated_ms() == 3000);
	REQUIRE(listener.lines.size() == 2);
	CHECK(listener.lines[1] == "Kitchen.Temperature 20.5 -> 20.5 (stale, good -> stale, changed)");
	CHECK(f.model.report_stale(f.knx, 1) == DatapointStatus::ok);
	CHECK(listener.lines.size() == 2);
	// The next report makes it good again.
	f.knx.receive(1, Value::of_real(20.5));
	CHECK(f.model.get(1)->quality() == Quality::good);
	CHECK(listener.lines.back() == "Kitchen.Temperature 20.5 -> 20.5 (report, stale -> good, changed)");
	LoopbackTechnology other{f.model, f.loop};
	CHECK(f.model.report_stale(other, 1) == DatapointStatus::not_owner);
	CHECK(f.model.report_stale(f.knx, 99) == DatapointStatus::unknown_id);
}

TEST_CASE("DatapointModel and the DPT codecs: a telegram becomes a datapoint value and back") {
	Fixture f;
	// What the KNX adapter of issue #7 does with a received group write for 9.001.
	hc::Dpt dpt{9, 1};
	REQUIRE(dpt.value_type() == f.model.get(1)->type());
	const uint8_t telegram[] = {0x0C, 0x33};
	Value value;
	REQUIRE(hc::decode(dpt, telegram, sizeof(telegram), value));
	CHECK(f.knx.receive(1, value) == DatapointStatus::ok);
	CHECK(f.model.get(1)->value().as_real() == doctest::Approx(21.5));
	// And a command to transmit.
	REQUIRE(f.model.command(2, Value::of_real(-5.5)) == DatapointStatus::ok);
	hc::DptBytes bytes;
	REQUIRE(hc::encode(dpt, f.knx.commands.back().value, bytes));
	CHECK(bytes.length == 2);
	CHECK(bytes.data[0] == 0x85);
	CHECK(bytes.data[1] == 0xDA);
}

TEST_CASE("DatapointModel memory per datapoint") {
	// The budget documented in doc/datapoints.md: the table entry plus 2 bytes of name index plus
	// the name with its NUL. 64 bytes on the 32-bit ESP, 72 on a 64-bit host (value, time, the
	// name, owner and transmitter pointers, id, type, flags, quality, padded to 8).
	CHECK(sizeof(Datapoint) <= 72);
	CHECK(sizeof(Datapoint) == (32 + 8 + 3 * sizeof(void*) + 5 + 7) / 8 * 8);
}

namespace {

/// An application component that owns datapoints and publishes them (the KNX time master's role):
/// it refuses the application's commands.
class Publisher : public hc::Technology {
public:
	const char* name() const override { return "publisher"; }

	bool command(const Datapoint&, const Value&) override {
		++commands;
		return false;
	}

	int commands = 0;
};

/// The owner publishes, a transmitter sends: ids 0 clock (time of day, readable and transmitting),
/// 1 local (transmits, no transmitter), 2 quiet (no kTransmit), 3 free (no owner).
struct PublishFixture {
	ScopedCapture capture;
	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model{clock};
	LoopbackTechnology bus{model, loop};
	Publisher owner;
	RecordingListener listener{"l"};

	PublishFixture() {
		const DatapointSpec specs[] = {
			{"clock", ValueType::time_of_day, DatapointFlags::kReadable | DatapointFlags::kTransmit},
			{"local", ValueType::real, DatapointFlags::kTransmit},
			{"quiet", ValueType::real, DatapointFlags::kReadable},
			{"free", ValueType::real, kRWT},
		};
		REQUIRE(model.load(specs, 4) == DatapointStatus::ok);
		for (uint16_t id = 0; id < 3; ++id) {
			REQUIRE(model.bind(id, owner) == DatapointStatus::ok);
		}
		REQUIRE(model.add_listener(listener) == DatapointStatus::ok);
	}
};

} // namespace

TEST_CASE("DatapointModel.SetTransmitterRules") {
	PublishFixture f;
	const char* error = nullptr;
	// No owner yet.
	CHECK(f.model.set_transmitter(3, f.bus, &error) == DatapointStatus::not_owner);
	CHECK(std::string(error) == "the datapoint has no owner yet");
	CHECK(f.capture.sink.contains("E hc.dp: refusing loopback as transmitter of 'free': the datapoint has no owner yet"));
	CHECK(f.model.set_transmitter(99, f.bus, &error) == DatapointStatus::unknown_id);
	// The owner as its own transmitter changes nothing.
	CHECK(f.model.set_transmitter(0, f.owner) == DatapointStatus::ok);
	CHECK(f.model.get(0)->transmitter() == nullptr);
	CHECK(f.model.set_transmitter(0, f.bus) == DatapointStatus::ok);
	CHECK(f.model.get(0)->transmitter() == &f.bus);
	CHECK(f.model.get(0)->technology() == &f.owner);
	CHECK(f.model.set_transmitter(0, f.bus) == DatapointStatus::ok); // again: no change
	struct Other : hc::Technology {
		const char* name() const override { return "other"; }
		bool command(const Datapoint&, const Value&) override { return true; }
	} other;
	CHECK(f.model.set_transmitter(0, other, &error) == DatapointStatus::not_owner);
	CHECK(std::string(error) == "the datapoint has another transmitter");
	CHECK(f.capture.sink.contains("refusing other as transmitter of 'clock': transmitted by loopback"));
	// A load drops it like the binding.
	const DatapointSpec specs[] = {{"clock", ValueType::time_of_day, DatapointFlags::kTransmit}};
	REQUIRE(f.model.load(specs, 1) == DatapointStatus::ok);
	CHECK(f.model.get(0)->transmitter() == nullptr);
	CHECK(f.model.get(0)->technology() == nullptr);
}

TEST_CASE("DatapointModel.PublishThroughTheTransmitter") {
	PublishFixture f;
	REQUIRE(f.model.set_transmitter(0, f.bus) == DatapointStatus::ok);
	hc::TimeOfDay t{3, 12, 10, 0};
	f.clock.now_ms = 4000;
	CHECK(f.model.publish(f.owner, 0, Value::of_time(t)) == DatapointStatus::ok);
	REQUIRE(f.bus.commands.size() == 1);
	CHECK(f.bus.commands[0].id == 0);
	CHECK(f.bus.commands[0].value.as_time() == t);
	const Datapoint& dp = *f.model.get(0);
	CHECK(dp.value().as_time() == t);
	CHECK(dp.quality() == Quality::good);
	CHECK(dp.updated_ms() == 4000);
	REQUIRE(f.listener.lines.size() == 1);
	CHECK(f.listener.lines[0] == "clock (none) -> 12:10:00 Wed (publish, unknown -> good, changed)");
	CHECK(f.owner.commands == 0); // the owner is not asked about its own publish

	// The transmitter refuses: nothing is stored.
	f.bus.accept = false;
	CHECK(f.model.publish(f.owner, 0, Value::of_time(hc::TimeOfDay{3, 12, 20, 0})) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("W hc.dp: publish 12:20:00 Wed of 'clock' refused by loopback"));
	CHECK(dp.value().as_time() == t);
	CHECK(f.listener.lines.size() == 1);
	// The transmitter may not call back into the model from its command.
	CHECK(f.bus.reentry == DatapointStatus::busy);
}

TEST_CASE("DatapointModel.PublishWithoutTransmitter") {
	PublishFixture f;
	// kTransmit without a transmitter, and a transmitter on a datapoint without kTransmit: stored only.
	CHECK(f.model.publish(f.owner, 1, Value::of_unsigned(7)) == DatapointStatus::ok);
	CHECK(f.model.get(1)->value().as_real() == doctest::Approx(7.0));
	REQUIRE(f.model.set_transmitter(2, f.bus) == DatapointStatus::ok);
	CHECK(f.model.publish(f.owner, 2, Value::of_real(1.5)) == DatapointStatus::ok);
	CHECK(f.bus.commands.empty());
	CHECK(f.listener.lines.back() == "quiet (none) -> 1.5 (publish, unknown -> good, changed)");
}

TEST_CASE("DatapointModel.PublishRefusals") {
	PublishFixture f;
	REQUIRE(f.model.set_transmitter(0, f.bus) == DatapointStatus::ok);
	// Only the owner publishes; the transmitter is not the owner.
	CHECK(f.model.publish(f.bus, 0, Value::of_time(hc::TimeOfDay{})) == DatapointStatus::not_owner);
	CHECK(f.capture.sink.contains("E hc.dp: refusing publish of 'clock' from loopback: not its owner"));
	CHECK(f.model.publish(f.owner, 0, Value::of_bool(true)) == DatapointStatus::wrong_type);
	CHECK(f.capture.sink.contains("refusing publish of 'clock' from publisher: a boolean value for a time of day datapoint"));
	CHECK(f.model.publish(f.owner, 9, Value::of_real(1)) == DatapointStatus::unknown_id);
	CHECK(f.bus.commands.empty());
	// The transmitter cannot report or mark stale what it does not own.
	CHECK(f.model.report(f.bus, 0, Value::of_time(hc::TimeOfDay{})) == DatapointStatus::not_owner);
	CHECK(f.model.report_stale(f.bus, 0) == DatapointStatus::not_owner);
	// The application's commands go to the owner, never to the transmitter.
	CHECK(f.model.command(0, Value::of_time(hc::TimeOfDay{})) == DatapointStatus::not_writable);
	const DatapointSpec specs[] = {{"clock", ValueType::time_of_day, kRWT}};
	REQUIRE(f.model.load(specs, 1) == DatapointStatus::ok);
	REQUIRE(f.model.bind(0, f.owner) == DatapointStatus::ok);
	REQUIRE(f.model.set_transmitter(0, f.bus) == DatapointStatus::ok);
	CHECK(f.model.command(0, Value::of_time(hc::TimeOfDay{})) == DatapointStatus::refused);
	CHECK(f.owner.commands == 1);
	CHECK(f.bus.commands.empty());
	// Not from a listener.
	f.listener.action = [&](const DatapointUpdate&) {
		CHECK(f.model.publish(f.owner, 0, Value::of_time(hc::TimeOfDay{})) == DatapointStatus::busy);
		CHECK(f.model.set_transmitter(0, f.bus) == DatapointStatus::busy);
	};
	CHECK(f.model.publish(f.owner, 0, Value::of_time(hc::TimeOfDay{1, 1, 1, 1})) == DatapointStatus::ok);
	CHECK(f.capture.sink.contains("refusing publish from a datapoint listener"));
}

namespace {

/// A technology that records the reads it is asked for and accepts them unless `accept` is false.
struct ReadingTechnology : hc::Technology {
	explicit ReadingTechnology(DatapointModel& model_) : model(model_) {}
	const char* name() const override { return "reading"; }
	bool command(const Datapoint&, const Value&) override { return true; }
	bool read(const Datapoint& datapoint) override {
		reads.push_back(datapoint.id());
		// The model refuses re-entry while the technology handles the read (the answer comes later).
		reentry = model.report(*this, datapoint.id(), Value::of_real(1));
		return accept;
	}
	DatapointModel& model;
	std::vector<uint16_t> reads;
	bool accept = true;
	DatapointStatus reentry = DatapointStatus::ok;
};

} // namespace

TEST_CASE("DatapointModel.ReadGoesToTheOwningTechnology") {
	Fixture f;
	ReadingTechnology reading(f.model);
	REQUIRE(f.model.bind(4, reading) == DatapointStatus::ok);
	CHECK(f.model.read(4) == DatapointStatus::ok);
	CHECK(reading.reads == std::vector<uint16_t>{4});
	CHECK(reading.reentry == DatapointStatus::busy);
	CHECK(f.model.get(4)->quality() == Quality::unknown); // the read itself changes nothing
	reading.accept = false;
	CHECK(f.model.read(4) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("read of 'Status.Text' refused by reading"));
	// The default of a technology: no reads.
	CHECK(f.model.read(0) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("read of 'Kitchen.Light' refused by loopback"));
	// No technology, no datapoint.
	CHECK(f.model.read(3) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("refusing read of 'Rule.Enabled': no technology owns it"));
	CHECK(f.model.read(99) == DatapointStatus::unknown_id);
	// Not from within a listener.
	RecordingListener listener("l");
	DatapointStatus from_listener = DatapointStatus::ok;
	listener.action = [&](const DatapointUpdate&) { from_listener = f.model.read(4); };
	f.model.add_listener(listener);
	CHECK(f.knx.receive(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(from_listener == DatapointStatus::busy);
	CHECK(reading.reads.size() == 2);
}
