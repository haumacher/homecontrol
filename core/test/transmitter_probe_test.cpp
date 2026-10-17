/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the owner/transmitter split of the datapoint model (the
// KNX time master package): a publish through a refusing and an accepting transmitter, the roles
// after a reload, and what each role may and may not do.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/technology.h"
#include "test_support.h"

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

namespace {

struct Tech : hc::Technology {
	explicit Tech(const char* n) : name_(n) {}
	const char* name() const override { return name_; }
	bool command(const hc::Datapoint& dp, const Value& value) override {
		commands.push_back({dp.id(), value});
		return accept;
	}
	const char* name_;
	bool accept = true;
	std::vector<std::pair<uint16_t, Value>> commands;
};

struct Origins : DatapointListener {
	void datapoint_updated(const DatapointUpdate& u) override { origins.push_back(hc::update_origin_name(u.origin)); }
	std::vector<std::string> origins;
};

} // namespace

TEST_CASE("TransmitterProbe.PublishStoresOnlyWhatTheTransmitterAccepted") {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"knx.time", ValueType::time_of_day, DatapointFlags::kReadable | DatapointFlags::kTransmit},
		{"plain", ValueType::boolean, DatapointFlags::kReadable | DatapointFlags::kTransmit}};
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	Tech master("master"), bus("bus"), other("other");
	Origins listener;
	REQUIRE(model.add_listener(listener) == DatapointStatus::ok);
	const char* error = nullptr;
	// Roles: an owner first, then one transmitter; a second transmitter and a transmitter without
	// an owner are refused.
	CHECK(model.set_transmitter(0, bus, &error) != DatapointStatus::ok);
	REQUIRE(model.bind(0, master) == DatapointStatus::ok);
	REQUIRE(model.set_transmitter(0, bus, &error) == DatapointStatus::ok);
	CHECK(model.set_transmitter(0, other, &error) != DatapointStatus::ok);
	CHECK(model.set_transmitter(0, bus, &error) == DatapointStatus::ok);    // the same again is a no-op
	CHECK(model.set_transmitter(0, master, &error) == DatapointStatus::ok); // naming the owner changes nothing
	hc::TimeOfDay t{1, 12, 10, 0};
	// The bus is down: the transmitter refuses, nothing is stored, nobody is notified.
	bus.accept = false;
	CHECK(model.publish(master, 0, Value::of_time(t)) == DatapointStatus::refused);
	CHECK(model.get(0)->quality() == Quality::unknown);
	CHECK(listener.origins.empty());
	CHECK(bus.commands.size() == 1);
	// Up again: stored with the publish origin, the transmitter saw it first.
	bus.accept = true;
	CHECK(model.publish(master, 0, Value::of_time(t)) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::good);
	CHECK(model.get(0)->value().as_time() == t);
	REQUIRE(listener.origins.size() == 1);
	CHECK(listener.origins[0] == "publish");
	CHECK(bus.commands.size() == 2);
	// Only the owner publishes, reports or marks stale; the transmitter is not the owner.
	CHECK(model.publish(bus, 0, Value::of_time(t)) == DatapointStatus::not_owner);
	CHECK(model.report(bus, 0, Value::of_time(t)) == DatapointStatus::not_owner);
	CHECK(model.report_stale(bus, 0) == DatapointStatus::not_owner);
	CHECK(model.report_stale(master, 0) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::stale);
	// A command from the application goes to the owner, never to the transmitter.
	size_t bus_before = bus.commands.size();
	CHECK(model.command(0, Value::of_time(t)) == DatapointStatus::not_writable); // not writable: refused by the model
	CHECK(bus.commands.size() == bus_before);
	CHECK(master.commands.empty());
	// A datapoint without a transmitter publishes without a bus.
	REQUIRE(model.bind(1, master) == DatapointStatus::ok);
	CHECK(model.publish(master, 1, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(model.get(1)->value() == Value::of_bool(true));
	// A reload drops both roles.
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	CHECK(model.publish(master, 0, Value::of_time(t)) == DatapointStatus::not_owner);
	REQUIRE(model.bind(0, master) == DatapointStatus::ok);
	CHECK(model.publish(master, 0, Value::of_time(t)) == DatapointStatus::ok); // no transmitter: stored only
	CHECK(bus.commands.size() == bus_before);
	CHECK(capture.sink.count("E hc.") + capture.sink.count("W hc.") >= 1); // the refusals spoke
}
