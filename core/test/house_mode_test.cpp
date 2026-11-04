/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The house mode (spec/blocks.md 19) on the device's block runtime: every transition and its guard,
// the persisted mode across reboots, the HVAC mapping, the chain to a room controller, its cost. The
// contract cases of the same behaviors are in spec/fixtures/blocks/.
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "allocation_counter.h"
#include "block_rig.h"
#include "doctest.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/house_mode.h"
#include "hc/tz.h"
#include "hc/value_store.h"

using hc::blocks::HouseMode;

namespace {

using hc_test::blocks::kHour;
using hc_test::blocks::kMinute;
using hc_test::blocks::Rig;

using Points = std::vector<std::pair<std::string, hc::ValueType>>;

Points house_datapoints() {
	Points p;
	for (const char* b : {"pres", "pres2", "awayB", "nightB", "nightB2", "homeB", "vac", "morn", "eve", "occ"}) {
		p.emplace_back(b, hc::ValueType::boolean);
	}
	p.emplace_back("hvac", hc::ValueType::enumeration);
	p.emplace_back("temp", hc::ValueType::real);
	p.emplace_back("temp2", hc::ValueType::real);
	p.emplace_back("valve", hc::ValueType::real);
	return p;
}

constexpr const char* kAllInputs = R"("presence": ["pres", "pres2"], "awayButton": "awayB",
	"nightButton": ["nightB", "nightB2"], "homeButton": "homeB", "vacation": "vac", "morning": "morn",
	"evening": "eve")";

std::string house(const std::string& parameters, const std::string& inputs = kAllInputs,
	const std::string& more = "") {
	return R"([{"id": "hm", "type": "org.homecontrol.house-mode", "version": "1.0", "label": "House",
		"inputs": {)" + inputs + R"(}, "outputs": {"hvacMode": "hvac", "occupied": "occ"},
		"parameters": {)" + parameters + "}}" + more + "]";
}

/// The rig with the house mode of `parameters` (awayDelay 10 min unless given).
Rig rig_of(const std::string& parameters = R"("awayDelay": 600000)", const std::string& inputs = kAllInputs,
	hc_test::FakeBlobStore* blobs = nullptr) {
	return Rig(house(parameters, inputs), blobs, house_datapoints());
}

double mode(Rig& rig) { return rig.number("block/hm/mode"); }

void flag(Rig& rig, const char* name, bool value) { rig.report(name, hc::Value::of_bool(value)); }

/// A press of a button a second after the previous event: its datapoint reports true (a press needs an
/// update time later than the button's latest, 19.2 D6).
void press(Rig& rig, const char* button) {
	rig.run(rig.now() + 1000);
	flag(rig, button, true);
}

/// Every output of the mode `m` as 19.5 writes it, with the default mapping.
void check_outputs(Rig& rig, double m, double hvac) {
	CHECK(mode(rig) == m);
	CHECK(rig.number("hvac") == hvac);
	CHECK(rig.number("block/hm/hvacMode") == hvac);
	CHECK(rig.number("block/hm/home") == (m == HouseMode::home ? 1 : 0));
	CHECK(rig.number("block/hm/away") == (m == HouseMode::away ? 1 : 0));
	CHECK(rig.number("block/hm/night") == (m == HouseMode::night ? 1 : 0));
	CHECK(rig.number("block/hm/onVacation") == (m == HouseMode::vacation ? 1 : 0));
	CHECK(rig.number("occ") == (m == HouseMode::home || m == HouseMode::night ? 1 : 0));
}

} // namespace

TEST_CASE("HouseMode: the manifest is valid and the type is built in") {
	CHECK(hc::blocks::check_manifest(HouseMode::instance().manifest()) == nullptr);
	CHECK(hc::blocks::BlockRegistry::device().find("org.homecontrol.house-mode", 1) == &HouseMode::instance());
	CHECK(HouseMode::instance().manifest().persists());
}

TEST_CASE("HouseMode: away by absence after awayDelay; presence cancels the delay; unknown and stale are no evidence") {
	Rig rig = rig_of();
	check_outputs(rig, HouseMode::home, 1);
	rig.run(2 * kHour); // presence unknown: no evidence, home stays
	CHECK(mode(rig) == HouseMode::home);

	flag(rig, "pres", false);
	uint64_t absent = rig.now();
	rig.run(absent + 9 * kMinute);
	CHECK(mode(rig) == HouseMode::home);
	flag(rig, "pres2", true); // presence cancels the delay
	flag(rig, "pres2", false);
	uint64_t again = rig.now();
	rig.run(again + 10 * kMinute - 1);
	CHECK(mode(rig) == HouseMode::home);
	rig.run(again + 10 * kMinute); // the timer `away`
	check_outputs(rig, HouseMode::away, 2);

	flag(rig, "pres", true); // return detection (armed by the absence that sent the house away)
	check_outputs(rig, HouseMode::home, 1);
	flag(rig, "pres", false);
	rig.run(rig.now() + 5 * kMinute);
	rig.stale("pres"); // stale: the aggregate over the stale ones is stale, no evidence
	rig.stale("pres2");
	rig.run(rig.now() + kHour);
	CHECK(mode(rig) == HouseMode::home);
	flag(rig, "pres", false); // good again: the delay starts anew
	rig.run(rig.now() + 10 * kMinute);
	CHECK(mode(rig) == HouseMode::away);
}

TEST_CASE("HouseMode: return detection after the away button waits for absence; off, only the home button returns") {
	Rig rig = rig_of();
	flag(rig, "pres", true);
	press(rig, "awayB");
	check_outputs(rig, HouseMode::away, 2);
	flag(rig, "pres", true); // the leavers pass the hall's detector: not armed yet
	rig.run(rig.now() + kHour);
	CHECK(mode(rig) == HouseMode::away);
	CHECK(rig.field(HouseMode::kArmed) == 0);
	flag(rig, "pres", false); // absence arms
	CHECK(rig.field(HouseMode::kArmed) == 1);
	flag(rig, "pres", true);
	CHECK(mode(rig) == HouseMode::home);

	flag(rig, "awayB", false); // a release is no press
	CHECK(mode(rig) == HouseMode::home);
	press(rig, "awayB"); // the same true value again is a press
	CHECK(mode(rig) == HouseMode::away);

	rig.set("returnDetection", 0);
	flag(rig, "pres", false);
	flag(rig, "pres", true);
	rig.run(rig.now() + kHour);
	CHECK(mode(rig) == HouseMode::away);
	press(rig, "homeB");
	CHECK(mode(rig) == HouseMode::home);
	rig.stale("homeB"); // going stale is no press
	rig.set("returnDetection", 1);
	press(rig, "awayB");
	rig.stale("awayB");
	CHECK(mode(rig) == HouseMode::away);
}

TEST_CASE("HouseMode: the night by the button and by the evening while home; the morning ends only the night") {
	Rig rig = rig_of();
	flag(rig, "eve", true); // unknown -> true (a schedule output restored): no moment
	CHECK(mode(rig) == HouseMode::home);
	flag(rig, "eve", false);
	flag(rig, "morn", false);
	flag(rig, "eve", true); // the evening while home
	check_outputs(rig, HouseMode::night, 3);

	flag(rig, "pres", false); // sleepers do not move: no away at night
	rig.run(rig.now() + 8 * kHour);
	CHECK(mode(rig) == HouseMode::night);
	flag(rig, "pres", true); // somebody up: still night
	flag(rig, "pres", false);
	CHECK(mode(rig) == HouseMode::night);

	flag(rig, "morn", true); // the morning ends the night and restarts the delay (D4)
	uint64_t morning = rig.now();
	check_outputs(rig, HouseMode::home, 1);
	rig.run(morning + 10 * kMinute - 1);
	CHECK(mode(rig) == HouseMode::home);
	rig.run(morning + 10 * kMinute);
	CHECK(mode(rig) == HouseMode::away);

	flag(rig, "eve", false);
	flag(rig, "eve", true); // the evening while away: nothing
	CHECK(mode(rig) == HouseMode::away);
	press(rig, "nightB2"); // the night button from away (the second datapoint of the button)
	CHECK(mode(rig) == HouseMode::night);
	flag(rig, "morn", false);
	press(rig, "homeB"); // the home button ends the night
	CHECK(mode(rig) == HouseMode::home);
	press(rig, "nightB");
	CHECK(mode(rig) == HouseMode::night);
	press(rig, "awayB"); // the away button from night
	CHECK(mode(rig) == HouseMode::away);
	flag(rig, "morn", true); // the morning while away: nothing
	CHECK(mode(rig) == HouseMode::away);
}

TEST_CASE("HouseMode: vacation overrides every mode and the buttons; it ends by the input, unknown keeps it") {
	hc_test::FakeBlobStore blobs;
	{
		Rig rig = rig_of(R"("awayDelay": 600000)", kAllInputs, &blobs);
		for (const char* into : {"homeB", "awayB", "nightB"}) {
			press(rig, into);
			flag(rig, "vac", true);
			check_outputs(rig, HouseMode::vacation, 4);
			press(rig, "homeB"); // ignored on vacation, and not kept for later
			press(rig, "awayB");
			flag(rig, "pres", true);
			CHECK(mode(rig) == HouseMode::vacation);
			flag(rig, "vac", false);
			check_outputs(rig, HouseMode::home, 1);
		}
		flag(rig, "pres", false);
		flag(rig, "vac", true);
		rig.stale("vac"); // stale keeps
		CHECK(mode(rig) == HouseMode::vacation);
		rig.reboot(); // unknown after the reboot keeps the persisted vacation
		CHECK(mode(rig) == HouseMode::vacation);
		CHECK(rig.number("hvac") == 4);
		press(rig, "homeB");
		CHECK(mode(rig) == HouseMode::vacation);
		rig.reboot();
	}
	// An image that no longer binds `vacation` ends a persisted vacation (D8).
	Rig rig = rig_of(R"("awayDelay": 600000)", R"("presence": "pres")", &blobs);
	CHECK(mode(rig) == HouseMode::home);
	CHECK(rig.number("hvac") == 1);
}

TEST_CASE("HouseMode: a reboot with unknown presence keeps each persisted mode; the inputs re-evaluate it") {
	hc_test::FakeBlobStore blobs;
	Rig rig = rig_of(R"("awayDelay": 600000)", kAllInputs, &blobs);
	press(rig, "awayB");
	rig.reboot();
	check_outputs(rig, HouseMode::away, 2); // the start writes the mode again
	rig.run(rig.now() + 2 * kHour);
	CHECK(mode(rig) == HouseMode::away);
	flag(rig, "pres", true); // a start is armed: presence returns home
	CHECK(mode(rig) == HouseMode::home);

	press(rig, "nightB");
	rig.reboot();
	flag(rig, "pres", false);
	rig.run(rig.now() + 2 * kHour);
	check_outputs(rig, HouseMode::night, 3);

	press(rig, "homeB");
	flag(rig, "pres", false);
	rig.run(rig.now() + 9 * kMinute);
	rig.reboot(); // the delay counts from the start, never from before it
	uint64_t boot = rig.now();
	flag(rig, "pres", false);
	rig.run(boot + 9 * kMinute);
	CHECK(mode(rig) == HouseMode::home);
	rig.run(boot + 10 * kMinute);
	CHECK(mode(rig) == HouseMode::away);
}

TEST_CASE("HouseMode: the record is written at once after a boot, then paced by B13") {
	Rig rig = rig_of();
	rig.run(kMinute);
	size_t base = rig.blobs().writes; // none: a fresh start changes no persisted field
	CHECK(base == 0);
	press(rig, "awayB");
	CHECK(rig.blobs().writes == base + 1); // the first write after the boot: at once
	press(rig, "homeB");
	CHECK(rig.blobs().writes == base + 1);
	rig.run(rig.now() + 30 * 1000);
	CHECK(rig.blobs().writes == base + 1);
	rig.run(rig.now() + 2 * kMinute); // max(persistEvery 60 s, 1 x 120 s) after the previous
	CHECK(rig.blobs().writes == base + 2);
	rig.run(rig.now() + kHour); // nothing changed: nothing written
	CHECK(rig.blobs().writes == base + 2);
}

TEST_CASE("HouseMode: the HVAC mapping is a parameter per mode; the failsafe is home with the rooms on auto") {
	Rig rig = rig_of(R"("hvacHome": "auto", "hvacAway": "comfort", "hvacNight": 2, "hvacVacation": "night")");
	CHECK(rig.number("hvac") == 0);
	press(rig, "awayB");
	CHECK(rig.number("hvac") == 1);
	press(rig, "nightB");
	CHECK(rig.number("hvac") == 2);
	flag(rig, "vac", true);
	CHECK(rig.number("hvac") == 3);

	const char* reason = nullptr;
	REQUIRE(rig.blocks().set_enabled("hm", false, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	CHECK(rig.number("block/hm/mode") == 0); // the failsafe (D12)
	CHECK(rig.number("hvac") == 0);
	CHECK(rig.number("block/hm/home") == 1);
	CHECK(rig.number("block/hm/onVacation") == 0);
	CHECK(rig.number("occ") == 1);
	REQUIRE(rig.blocks().set_enabled("hm", true, &reason) == hc::blocks::ChangeStatus::ok);
	rig.run(rig.now());
	CHECK(mode(rig) == HouseMode::vacation); // the record in RAM; the start writes again
	CHECK(rig.number("hvac") == 3);
}

TEST_CASE("HouseMode: since is the local wall time of the change in minutes, dated once the clock is synchronized") {
	Rig rig = rig_of();
	CHECK(rig.model().find("block/hm/since")->quality() == hc::Quality::unknown); // no wall clock yet
	rig.run(5 * kMinute);
	const int64_t day = hc::days_from_civil(2026, 11, 4) * 1440; // in minutes
	rig.wall().start_utc = (day + 10 * 60 - 5) * 60 + 30; // 10:00:30 now
	rig.wall().synchronized = true;
	rig.run(6 * kMinute); // the period dates the fresh start: 09:55:30
	CHECK(rig.number("block/hm/since") == static_cast<double>(day + 9 * 60 + 55));
	press(rig, "awayB"); // 10:01:31
	CHECK(rig.number("block/hm/since") == static_cast<double>(day + 10 * 60 + 1));
	rig.wall().synchronized = false;
	rig.run(rig.now() + 29 * 1000 + 500);
	press(rig, "homeB"); // 10:02:01.5, without a wall clock
	CHECK(rig.number("block/hm/since") == static_cast<double>(day + 10 * 60 + 1));
	rig.run(rig.now() + 20 * kMinute);
	rig.wall().synchronized = true;
	rig.run(rig.now() + kMinute); // the period at 10:22:30 takes it back by 1228.5 s: 10:02:01.5
	CHECK(rig.number("block/hm/since") == static_cast<double>(day + 10 * 60 + 2));
	rig.run(rig.now() + kMinute);
	press(rig, "awayB");
	rig.wall().synchronized = false;
	rig.run(rig.now() + kMinute);
	press(rig, "homeB"); // not dated before the reboot: undated until the next change
	rig.reboot();
	rig.wall().synchronized = true;
	rig.run(rig.now() + 5 * kMinute);
	CHECK(rig.field(HouseMode::kSinceField) == -1);
	CHECK(rig.model().find("block/hm/since")->quality() == hc::Quality::unknown);
}

TEST_CASE("HouseMode: a restored mode of a later minor version starts as home") {
	hc_test::FakeBlobStore blobs;
	{
		Rig rig = rig_of(R"("awayDelay": 600000)", kAllInputs, &blobs);
		press(rig, "awayB");
		rig.reboot();
	}
	// The record in the store is the device's (spec/blocks.md 8.1): patch the mode's double to 4.
	REQUIRE(blobs.blobs.size() == 1);
	std::vector<uint8_t>& record = blobs.blobs.begin()->second;
	const double away = 1;
	const uint8_t* bits = reinterpret_cast<const uint8_t*>(&away);
	auto at = std::search(record.begin(), record.end(), bits, bits + sizeof(double));
	REQUIRE(at != record.end());
	const double later = 4;
	std::memcpy(&*at, &later, sizeof(double));
	uint32_t crc = hc::persist::crc32(0, record.data(), record.size() - 4);
	std::memcpy(record.data() + record.size() - 4, &crc, 4);
	Rig rig = rig_of(R"("awayDelay": 600000)", kAllInputs, &blobs);
	hc::blocks::BlockDiagnostics d;
	REQUIRE(rig.blocks().diagnostics(0, d));
	CHECK(d.status == hc::blocks::BlockStatus::running);
	CHECK(d.restored);
	check_outputs(rig, HouseMode::home, 1);
}

TEST_CASE("HouseMode: its hvacMode sets a room controller's setpoint; occupied its presence") {
	Rig rig(house(R"("awayDelay": 600000)", kAllInputs,
				R"(, {"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
				"inputs": {"temperature": "temp", "mode": "block/hm/hvacMode", "presence": "block/hm/occupied"},
				"outputs": {"valve": "valve"}})"),
		nullptr, house_datapoints());
	rig.report("temp", 18.0);
	CHECK(rig.number("block/wz/activeSetpoint") == 21);
	press(rig, "awayB");
	CHECK(rig.number("block/wz/activeSetpoint") == 19);
	press(rig, "nightB");
	CHECK(rig.number("block/wz/activeSetpoint") == 17);
	flag(rig, "vac", true);
	CHECK(rig.number("block/wz/activeSetpoint") == 7);
	CHECK(rig.number("block/wz/state") == 3); // frost
	flag(rig, "vac", false);
	CHECK(rig.number("block/wz/activeSetpoint") == 21);
}

TEST_CASE("HouseMode: no allocation in steady state; the cost of an instance is at most a room controller's") {
	Rig rig = rig_of();
	Rig room(R"([{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
		"inputs": {"temperature": ["temp", "temp2"], "window": "pres"}, "outputs": {"valve": "valve", "switch": "occ"}}])",
		nullptr, house_datapoints());
	flag(rig, "eve", false);
	flag(rig, "morn", false);
	press(rig, "awayB"); // the record is stored before the measurement
	rig.run(rig.now() + 10 * kMinute);
	{
		hc_test::AllocationScope scope;
		auto writes = rig.blobs().writes;
		uint16_t pres = rig.model().find("pres")->id();
		uint16_t away = rig.model().find("awayB")->id();
		uint16_t eve = rig.model().find("eve")->id();
		uint16_t morn = rig.model().find("morn")->id();
		size_t refused = 0;
		for (int i = 0; i < 120; ++i) {
			refused += rig.model().report(rig.technology(), pres, hc::Value::of_bool(i % 7 < 3)) != hc::DatapointStatus::ok;
			if (i % 20 == 0) {
				refused += rig.model().report(rig.technology(), away, hc::Value::of_bool(true)) != hc::DatapointStatus::ok;
			}
			refused += rig.model().report(rig.technology(), i % 40 < 20 ? eve : morn, hc::Value::of_bool(i % 2 == 0)) !=
				hc::DatapointStatus::ok;
			rig.run(rig.now() + 5 * kMinute);
		}
		size_t stored = static_cast<size_t>(rig.blobs().writes - writes);
		CHECK(refused == 0);
		CHECK(stored >= 1);
		CHECK(scope.count() - stored == 0); // the fake store allocates for each record it keeps
	}
	auto heap = [](Rig& r) {
		hc::blocks::BlockMemory m = r.blocks().memory();
		size_t bytes = m.runtime + m.set;
		for (size_t i = 0; i < r.model().size(); ++i) {
			const char* name = r.model().get(static_cast<uint16_t>(i))->name();
			if (std::strncmp(name, "block/", 6) == 0) {
				bytes += sizeof(hc::Datapoint) + std::strlen(name) + 1;
			}
		}
		return bytes;
	};
	size_t house_heap = heap(rig);
	size_t room_heap = heap(room);
	MESSAGE("heap per instance (host, 64-bit): house mode " << house_heap << " B, room controller " << room_heap << " B");
	CHECK(house_heap <= room_heap);
}
