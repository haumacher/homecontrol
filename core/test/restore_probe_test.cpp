/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the persistence package (issue #21): the model's restore()
// seen from a rule-like listener and from the owning technology.

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
using hc::Value;
using hc::ValueType;

namespace {

struct Tech : hc::Technology {
	const char* name() const override { return "bus"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

struct Counting : DatapointListener {
	void datapoint_updated(const DatapointUpdate& u) override { origins.push_back(hc::update_origin_name(u.origin)); }
	std::vector<std::string> origins;
};

} // namespace

TEST_CASE("RestoreProbe.RestoredValuesAreStaleSilentAndLoseAgainstTheBus") {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	DatapointModel model(clock);
	DatapointSpec specs[] = {{"temp", ValueType::real, DatapointFlags::kReadable | DatapointFlags::kPersist},
		{"count", ValueType::unsigned_int, DatapointFlags::kReadable | DatapointFlags::kPersist}};
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	Tech bus;
	Counting listener;
	REQUIRE(model.add_listener(listener) == DatapointStatus::ok);
	REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
	REQUIRE(model.bind(1, bus) == DatapointStatus::ok);
	// A restore at boot: stale, no listener fires (no rule triggers on a saved value), time 0.
	CHECK(model.restore(0, Value::of_real(20.5)) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::stale);
	CHECK(model.get(0)->value().as_real() == doctest::Approx(20.5));
	CHECK(model.get(0)->updated_ms() == 0);
	CHECK(listener.origins.empty());
	// An exact conversion restores, an inexact one is refused and leaves the datapoint unknown.
	CHECK(model.restore(1, Value::of_real(21.5)) == DatapointStatus::wrong_type);
	CHECK(model.get(1)->quality() == Quality::unknown);
	CHECK(model.restore(1, Value::of_signed(7)) == DatapointStatus::ok);
	CHECK(model.get(1)->value().as_unsigned() == 7);
	// Once the bus has spoken, a restore never overwrites it; and a restore cannot repeat either.
	clock.advance(500);
	REQUIRE(model.report(bus, 0, Value::of_real(21.0)) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::good);
	CHECK(model.restore(0, Value::of_real(19.0)) == DatapointStatus::busy);
	CHECK(model.get(0)->value().as_real() == doctest::Approx(21.0));
	CHECK(model.restore(1, Value::of_signed(8)) == DatapointStatus::busy); // stale counts as "has a value"
	CHECK(model.get(1)->value().as_unsigned() == 7);
	// The bus report was the only notification.
	REQUIRE(listener.origins.size() == 1);
	CHECK(listener.origins[0] == "report");
	// Unknown ids and a reload (which forgets restored values) are handled.
	CHECK(model.restore(9, Value::of_real(1.0)) == DatapointStatus::unknown_id);
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	CHECK(model.get(0)->quality() == Quality::unknown);
	CHECK(model.restore(0, Value::of_real(20.5)) == DatapointStatus::ok);
	// The flags are distinct bits and part of kAll.
	CHECK((DatapointFlags::kPersist & DatapointFlags::kHistory) == 0);
	CHECK((DatapointFlags::kAll & DatapointFlags::kPersist) != 0);
	CHECK((DatapointFlags::kAll & DatapointFlags::kHistory) != 0);
}
