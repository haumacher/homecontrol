/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the datapoint model and the DPT codecs (issue #6): a
// bus round trip through the codecs, listeners reacting through the event loop, a reload under
// listeners, exact and inexact conversions, and the quality life cycle.

#include <cmath>
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
using hc::Dpt;
using hc::DptBytes;
using hc::DptStatus;
using hc::Quality;
using hc::UpdateOrigin;
using hc::Value;
using hc::ValueType;
using hc_test::FakeClock;
using hc_test::ScopedCapture;

namespace {

constexpr uint8_t kRWT = DatapointFlags::kReadable | DatapointFlags::kWritable | DatapointFlags::kTransmit;

/// A technology that frames every accepted command with a DPT, as the KNX adapter will, and can
/// hand the frame back as a report after decoding it (the bus echo).
class CodecTechnology : public hc::Technology {
public:
	CodecTechnology(DatapointModel& model, Dpt dpt) : model_(model), dpt_(dpt) {}
	const char* name() const override { return "codec"; }
	bool command(const Datapoint& datapoint, const Value& value) override {
		DptBytes bytes;
		hc::DptResult r = hc::encode(dpt_, value, bytes);
		if (!r) {
			last_error = r.reason;
			return false;
		}
		frames.push_back(std::vector<uint8_t>(bytes.data, bytes.data + bytes.length));
		last_id = datapoint.id();
		return true;
	}
	/// Decodes the last frame and reports it, as the bus echo would.
	DatapointStatus echo() {
		Value v;
		hc::DptResult r = hc::decode(dpt_, frames.back().data(), frames.back().size(), v);
		REQUIRE(r);
		return model_.report(*this, last_id, v);
	}
	std::vector<std::vector<uint8_t>> frames;
	std::string last_error;
	uint16_t last_id = 0;

private:
	DatapointModel& model_;
	Dpt dpt_;
};

struct Recording : DatapointListener {
	void datapoint_updated(const DatapointUpdate& u) override {
		char buffer[64];
		updates.push_back(std::string(u.datapoint.name()) + ":" + hc::update_origin_name(u.origin) + ":"
			+ u.datapoint.value().format(buffer, sizeof(buffer)) + (u.changed() ? "" : ":same"));
		if (react) {
			react(u);
		}
	}
	std::vector<std::string> updates;
	std::function<void(const DatapointUpdate&)> react;
};

} // namespace

TEST_CASE("DatapointProbe.PercentRoundTripThroughDpt5001IsLossyOnlyByItsResolution") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"dimmer", ValueType::real, kRWT}};
	REQUIRE(model.load(specs, 1) == DatapointStatus::ok);
	CodecTechnology knx(model, Dpt{5, 1});
	REQUIRE(model.bind(0, knx) == DatapointStatus::ok);
	Recording rec;
	REQUIRE(model.add_listener(rec) == DatapointStatus::ok);

	REQUIRE(model.command(0, Value::of_real(33.3)) == DatapointStatus::ok);
	REQUIRE(knx.frames.size() == 1);
	CHECK(knx.frames[0] == std::vector<uint8_t>{85}); // 33.3 % of 255 = 84.9 -> 85
	clock.advance(10);
	REQUIRE(knx.echo() == DatapointStatus::ok);
	double v = model.get(0)->value().as_real();
	CHECK(std::fabs(v - 33.3) < 100.0 / 255.0); // within one step of the 8-bit scale
	CHECK(model.get(0)->updated_ms() == clock.now_ms);
	CHECK(rec.updates.size() == 2);
	CHECK(rec.updates[0].rfind("dimmer:command:", 0) == 0);
	CHECK(rec.updates[1].rfind("dimmer:report:", 0) == 0);

	// Out of range is refused by the codec, so the technology refuses, so the value stays.
	CHECK(model.command(0, Value::of_real(100.5)) == DatapointStatus::refused);
	CHECK(knx.frames.size() == 1);
	CHECK(std::fabs(model.get(0)->value().as_real() - v) < 1e-9);
	CHECK(rec.updates.size() == 2);
	CHECK_FALSE(knx.last_error.empty());
}

TEST_CASE("DatapointProbe.AListenerReactsThroughTheEventLoopNotSynchronously") {
	ScopedCapture capture;
	FakeClock clock;
	hc::EventLoop loop;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"presence", ValueType::boolean, kRWT}, {"light", ValueType::boolean, kRWT}};
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	CodecTechnology knx(model, Dpt{1, 1});
	REQUIRE(model.bind(0, knx) == DatapointStatus::ok);
	REQUIRE(model.bind(1, knx) == DatapointStatus::ok);

	// The rule: presence on -> light on. Done wrong (synchronously) it is refused; done right it posts.
	struct Rule {
		DatapointModel& model;
		hc::EventLoop& loop;
		DatapointStatus sync_result = DatapointStatus::ok;
		void fire() { model.command(1, Value::of_bool(true)); }
	} rule{model, loop};
	Recording rec;
	rec.react = [&](const DatapointUpdate& u) {
		if (u.datapoint.id() == 0 && u.datapoint.value().as_bool()) {
			rule.sync_result = model.command(1, Value::of_bool(true)); // wrong: inside the notification
			loop.post(hc::Callback::member<Rule, &Rule::fire>(&rule));   // right
		}
	};
	REQUIRE(model.add_listener(rec) == DatapointStatus::ok);

	REQUIRE(model.report(knx, 0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(rule.sync_result == DatapointStatus::busy);
	CHECK(capture.sink.count("E hc.dp") + capture.sink.count("E hc.datapoint") + capture.sink.count("E hc.model") >= 1);
	CHECK(model.get(1)->quality() == Quality::unknown); // nothing happened yet
	loop.run_once(clock.now_ms);
	CHECK(model.get(1)->value() == Value::of_bool(true));
	CHECK(knx.frames.size() == 1); // the light's command went to the bus
	bool small = knx.frames[0].size() <= 1; // DPT 1 is embedded (6-bit): at most one octet
	CHECK(small);
}

TEST_CASE("DatapointProbe.ReloadUnderListenersAndARefusedReloadKeepsTheOldModel") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec first[] = {{"a", ValueType::unsigned_int, kRWT}, {"b", ValueType::unsigned_int, kRWT}, {"c", ValueType::unsigned_int, kRWT}};
	REQUIRE(model.load(first, 3) == DatapointStatus::ok);
	Recording rec;
	REQUIRE(model.add_listener(rec) == DatapointStatus::ok);
	CodecTechnology knx(model, Dpt{5, 10});
	REQUIRE(model.bind(2, knx) == DatapointStatus::ok);
	REQUIRE(model.report(knx, 2, Value::of_unsigned(7)) == DatapointStatus::ok);

	// A smaller model replaces it; the old ids beyond the new size are gone, the listener stays.
	DatapointSpec second[] = {{"c", ValueType::unsigned_int, kRWT}};
	REQUIRE(model.load(second, 1) == DatapointStatus::ok);
	CHECK(model.size() == 1);
	CHECK(model.get(2) == nullptr);
	CHECK(model.find("c") != nullptr);
	CHECK(model.find("c")->id() == 0);
	CHECK(model.get(0)->quality() == Quality::unknown); // a load starts fresh
	CHECK(model.report(knx, 0, Value::of_unsigned(1)) == DatapointStatus::not_owner); // bindings do not survive a load
	REQUIRE(model.bind(0, knx) == DatapointStatus::ok);
	REQUIRE(model.report(knx, 0, Value::of_unsigned(1)) == DatapointStatus::ok);
	CHECK(rec.updates.size() == 2);

	// A refused load (duplicate names) keeps the current model and its values.
	DatapointSpec bad[] = {{"x", ValueType::boolean, kRWT}, {"x", ValueType::boolean, kRWT}};
	CHECK(model.load(bad, 2) == DatapointStatus::invalid_model);
	CHECK(model.size() == 1);
	CHECK(model.get(0)->value() == Value::of_unsigned(1));
	CHECK(capture.sink.count("E hc.") >= 1);

	// A name of 64 bytes and a type of none are refused too.
	std::string longname(64, 'n');
	DatapointSpec bad2[] = {{longname.c_str(), ValueType::boolean, kRWT}};
	CHECK(model.load(bad2, 1) == DatapointStatus::invalid_model);
	DatapointSpec bad3[] = {{"ok", ValueType::none, kRWT}};
	CHECK(model.load(bad3, 1) == DatapointStatus::invalid_model);
	CHECK(model.size() == 1);
}

TEST_CASE("DatapointProbe.ExactConversionsPassInexactOnesAreRefused") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"temp", ValueType::real, kRWT}, {"count", ValueType::unsigned_int, kRWT},
		{"mode", ValueType::enumeration, kRWT}, {"text", ValueType::string, kRWT}};
	REQUIRE(model.load(specs, 4) == DatapointStatus::ok);
	CodecTechnology knx(model, Dpt{9, 1});
	for (uint16_t id = 0; id < 4; ++id) {
		REQUIRE(model.bind(id, knx) == DatapointStatus::ok);
	}
	CHECK(model.report(knx, 0, Value::of_signed(-5)) == DatapointStatus::ok); // int -> real exact
	CHECK(model.get(0)->value().as_real() == -5.0);
	CHECK(model.report(knx, 1, Value::of_real(21.5)) == DatapointStatus::wrong_type); // 21.5 is no count
	CHECK(model.report(knx, 1, Value::of_real(21.0)) == DatapointStatus::ok);       // 21.0 is
	CHECK(model.get(1)->value().as_unsigned() == 21);
	CHECK(model.report(knx, 1, Value::of_signed(-1)) == DatapointStatus::wrong_type); // negative count
	CHECK(model.report(knx, 2, Value::of_unsigned(3)) == DatapointStatus::ok);        // enum from a number
	CHECK(model.report(knx, 3, Value::of_unsigned(3)) == DatapointStatus::wrong_type); // a number is no text
	std::string s28(28, 'x'), s29(29, 'x');
	CHECK(Value::of_string(s28.c_str()).has_value());
	CHECK_FALSE(Value::of_string(s29.c_str()).has_value());
	CHECK(model.report(knx, 3, *Value::of_string(s28.c_str())) == DatapointStatus::ok);
	CHECK(std::string(model.get(3)->value().as_string()) == s28);
}

TEST_CASE("DatapointProbe.QualityLifeCycleAndTimestamps") {
	ScopedCapture capture;
	FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"t", ValueType::real, DatapointFlags::kReadable}};
	REQUIRE(model.load(specs, 1) == DatapointStatus::ok);
	CodecTechnology knx(model, Dpt{9, 1});
	REQUIRE(model.bind(0, knx) == DatapointStatus::ok);
	Recording rec;
	REQUIRE(model.add_listener(rec) == DatapointStatus::ok);

	CHECK(model.get(0)->quality() == Quality::unknown);
	CHECK(model.command(0, Value::of_real(1.0)) == DatapointStatus::not_writable); // read-only
	clock.advance(1000);
	REQUIRE(model.report(knx, 0, Value::of_real(20.5)) == DatapointStatus::ok);
	uint64_t t1 = model.get(0)->updated_ms();
	CHECK(t1 == clock.now_ms);
	clock.advance(5000);
	REQUIRE(model.report_stale(knx, 0) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::stale);
	CHECK(model.get(0)->value() == Value::of_real(20.5)); // value kept
	REQUIRE(model.report_stale(knx, 0) == DatapointStatus::ok); // idempotent
	clock.advance(1000);
	REQUIRE(model.report(knx, 0, Value::of_real(20.5)) == DatapointStatus::ok); // same value, good again
	CHECK(model.get(0)->quality() == Quality::good);
	CHECK(model.get(0)->updated_ms() == clock.now_ms);
	CHECK(model.get(0)->updated_ms() > t1);
	// The listener saw: report (changed), stale (changed quality), report (changed quality). The
	// second report_stale on an already stale datapoint is a no-op: no notification while a bus
	// stays down.
	REQUIRE(rec.updates.size() == 3);
	CHECK(rec.updates[0].find(":same") == std::string::npos);
	CHECK(rec.updates[1].find(":same") == std::string::npos);
	CHECK(rec.updates[1].find(":stale:") != std::string::npos);
	CHECK(rec.updates[2].find(":same") == std::string::npos);
}

TEST_CASE("DptProbe.EmbeddedTypesFrameZeroOctetsAndTheirValueFitsSixBits") {
	Value v;
	DptBytes b;
	CHECK(Dpt{1, 1}.embedded());
	CHECK(Dpt{3, 7}.embedded());
	CHECK_FALSE(Dpt{18, 1}.embedded());
	CHECK_FALSE(Dpt{5, 1}.embedded());
	REQUIRE(hc::encode(Dpt{3, 7}, Value::of_controlled(true, 5), b));
	CHECK(b.length == 1);
	CHECK(b.data[0] == 0x0D); // c=1, stepcode 5 -> 0b1101, fits the 6-bit APCI field
	CHECK(b.data[0] < 64);
	REQUIRE(hc::decode(Dpt{3, 7}, b.data, 1, v));
	CHECK(v.as_controlled() == hc::Controlled{true, 5});
	CHECK(hc::encode(Dpt{3, 7}, Value::of_controlled(true, 8), b).status == DptStatus::out_of_range);
	// Two-byte float: 21.0 is 0x0C1A and -0.004 rounds to zero, not to -20.48 (E-Home's bug).
	REQUIRE(hc::encode(Dpt{9, 1}, Value::of_real(-0.004), b));
	CHECK(b.length == 2);
	CHECK(b.data[0] == 0x00);
	CHECK(b.data[1] == 0x00);
	uint8_t invalid[] = {0x7F, 0xFF};
	CHECK(hc::decode(Dpt{9, 1}, invalid, 2, v).status == DptStatus::invalid);
	// Wrong length is told apart from invalid content.
	uint8_t one[] = {0x0C};
	CHECK(hc::decode(Dpt{9, 1}, one, 1, v).status == DptStatus::wrong_length);
	// An unsupported subtype says so, and parse/format round-trips.
	CHECK_FALSE(Dpt{6, 20}.supported());
	CHECK(std::string(Dpt{6, 20}.unsupported_reason()).size() > 0);
	auto parsed = Dpt::parse("251.600");
	REQUIRE(parsed.has_value());
	char text[Dpt::kMaxText];
	CHECK(std::string(parsed->format(text, sizeof(text))) == "251.600");
}
