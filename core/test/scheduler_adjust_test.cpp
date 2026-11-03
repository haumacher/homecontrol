/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Adjustable schedule members (issue #58, spec/schedules.md 2.1, 4.2): the image declares members
// adjustable within bounds and a step; the inhabitant's values are overrides persisted by schedule id
// and member, kept or dropped by a new image; a change is not a gap and keeps a handled firing
// handled; the adjustable time is the datapoint schedule/<id>/time.
#include "doctest.h"
#include "scheduler_test_support.h"

using hc::ScheduleBound;
using hc::ScheduleChange;
using hc::ScheduleMember;

namespace {

constexpr int64_t kStep5 = 300;

/// The weekly 06:30 on workdays of the issue, `time` and `days` adjustable, the time bounded to
/// 05:00..09:00 with a step of 5 min.
struct Morning {
	ScheduleBound bound;
	ScheduleSpec spec;

	explicit Morning(int64_t min = 5 * 3600, int64_t max = 9 * 3600, uint16_t adjustable = ScheduleMember::kTime | ScheduleMember::kDays) {
		bound.member = ScheduleMember::kTime;
		bound.min = min;
		bound.max = max;
		bound.step = kStep5;
		spec = weekly("morning", kWorkdays, at(6, 30));
		spec.adjustable = adjustable;
		if ((adjustable & ScheduleMember::kTime) != 0) {
			spec.bounds = &bound;
			spec.bound_count = 1;
		}
	}

	Morning(const Morning& other) : bound(other.bound), spec(other.spec) {
		if (spec.bound_count > 0) {
			spec.bounds = &bound;
		}
	}
};

ScheduleSpec time_value(int hour, int minute, int second = 0) {
	ScheduleSpec values;
	values.time = at(hour, minute, second);
	return values;
}

ScheduleChange set_time(Rig& rig, int hour, int minute, int second = 0) {
	return rig.scheduler->set_members("morning", ScheduleMember::kTime, time_value(hour, minute, second));
}

} // namespace

TEST_CASE("ScheduleAdjust.WeeklyTimeWithBoundsAndStep") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0)); // Monday
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->adjustable(0) == (ScheduleMember::kTime | ScheduleMember::kDays));
	REQUIRE(rig.scheduler->bound(0, ScheduleMember::kTime) != nullptr);
	CHECK(rig.scheduler->bound(0, ScheduleMember::kTime)->step == kStep5);
	CHECK(rig.scheduler->bound(0, ScheduleMember::kDays) == nullptr);

	// 06:00 fires at 06:00 and is an override.
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kTime);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 6, 0));
	CHECK(rig.scheduler->image_spec(0).time.minute == 30);
	rig.run_until(cest(2026, 10, 12, 7, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 12, 6, 0)}});

	// Out of the bounds, off the step: refused, naming the member.
	{
		ScopedCapture capture;
		uint16_t culprit = 0;
		const char* reason = nullptr;
		CHECK(rig.scheduler->set_members("morning", ScheduleMember::kTime, time_value(4, 55), &culprit, &reason) ==
			ScheduleChange::out_of_bounds);
		CHECK(culprit == ScheduleMember::kTime);
		CHECK(std::string(reason) == "outside the member's bounds");
		CHECK(set_time(rig, 9, 5) == ScheduleChange::out_of_bounds);
		CHECK(set_time(rig, 6, 2) == ScheduleChange::out_of_bounds);
		CHECK(set_time(rig, 6, 5, 30) == ScheduleChange::out_of_bounds);
		CHECK(capture.sink.contains("refusing to change schedule morning: time: not on the member's step (out_of_bounds)"));
	}
	CHECK(rig.scheduler->spec(0).time.hour == 6);
	CHECK(rig.scheduler->spec(0).time.minute == 0);

	// The override survives a restart.
	rig.boot();
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kTime);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 13, 6, 0));
	rig.run_until(cest(2026, 10, 13, 7, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 13, 6, 0)}});

	// Setting the image's value back removes the override, also from the record.
	REQUIRE(set_time(rig, 6, 30) == ScheduleChange::ok);
	CHECK(rig.scheduler->overridden(0) == 0);
	char key[Scheduler::kRecordKeySize];
	Scheduler::record_key("morning", key);
	char record[128];
	REQUIRE(rig.settings.get_string(key, record, sizeof(record)) == hc::SettingsStatus::ok);
	CHECK(record[0] == '1'); // the record without overrides, readable by an older firmware
	rig.boot();
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == 0);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 14, 6, 30));
}

TEST_CASE("ScheduleAdjust.NotAdjustableAndMalformed") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning fixed(5 * 3600, 9 * 3600, ScheduleMember::kDays);
	REQUIRE(rig.load({fixed.spec}) == ScheduleStatus::ok);
	ScopedCapture capture;
	uint16_t culprit = 0;
	CHECK(rig.scheduler->set_members("morning", ScheduleMember::kTime, time_value(6, 0), &culprit) ==
		ScheduleChange::not_adjustable);
	CHECK(culprit == ScheduleMember::kTime);
	ScheduleSpec values;
	values.offset_s = 60;
	CHECK(rig.scheduler->set_members("morning", ScheduleMember::kOffset, values) == ScheduleChange::not_adjustable);
	values.days = 0;
	CHECK(rig.scheduler->set_members("morning", ScheduleMember::kDays, values) == ScheduleChange::malformed);
	CHECK(rig.scheduler->set_members("nope", ScheduleMember::kDays, values) == ScheduleChange::unknown);
	CHECK(capture.sink.contains("days must name 1..7 days of the week"));
}

TEST_CASE("ScheduleAdjust.ChangeIsNotAGap") {
	// At 06:15 the 06:30 moves to 06:00: no firing today and no "missed" line.
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 6, 15));
	ScopedCapture capture;
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 13, 6, 0));
	rig.run_until(cest(2026, 10, 12, 23, 0));
	CHECK(rig.take().empty());
	CHECK_FALSE(capture.sink.contains("missed"));
	rig.run_until(cest(2026, 10, 13, 7, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 13, 6, 0)}});
}

TEST_CASE("ScheduleAdjust.ChangeToLaterTodayFires") {
	// At 06:15 the 06:30 moves to 06:20: it fires at 06:20 today.
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 6, 15));
	REQUIRE(set_time(rig, 6, 20) == ScheduleChange::ok);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 6, 20));
	rig.run_until(cest(2026, 10, 12, 23, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 12, 6, 20)}});
}

TEST_CASE("ScheduleAdjust.FiredDayStaysHandled") {
	// After the firing at 06:30 the time moves to 07:00: no second firing today; and back, and
	// forward again, still none.
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 6, 45));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 12, 6, 30)}});
	REQUIRE(set_time(rig, 7, 0) == ScheduleChange::ok);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 13, 7, 0));
	rig.run_until(cest(2026, 10, 12, 6, 50));
	REQUIRE(set_time(rig, 6, 30) == ScheduleChange::ok);
	REQUIRE(set_time(rig, 8, 0) == ScheduleChange::ok);
	rig.run_until(cest(2026, 10, 12, 23, 0));
	CHECK(rig.take().empty());
	// The handled day survives a restart (the record keeps the latest handled moment).
	rig.boot();
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 13, 9, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 13, 8, 0)}});
}

TEST_CASE("ScheduleAdjust.DaysRemoveFriday") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 15, 12, 0)); // Thursday
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	ScheduleSpec values;
	values.days = 0x0F; // Monday .. Thursday
	REQUIRE(rig.scheduler->set_members("morning", ScheduleMember::kDays, values) == ScheduleChange::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kDays);
	rig.run_until(cest(2026, 10, 19, 7, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 19, 6, 30)}});
	// Days and time in one change, both persisted.
	values.days = Weekdays::kAll;
	values.time = at(7, 0);
	REQUIRE(rig.scheduler->set_members("morning", ScheduleMember::kDays | ScheduleMember::kTime, values) ==
		ScheduleChange::ok);
	rig.boot();
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == (ScheduleMember::kDays | ScheduleMember::kTime));
	CHECK(rig.scheduler->spec(0).days == Weekdays::kAll);
	// A refused member refuses the whole change.
	values.days = 0x01;
	values.time = at(4, 0);
	CHECK(rig.scheduler->set_members("morning", ScheduleMember::kDays | ScheduleMember::kTime, values) ==
		ScheduleChange::out_of_bounds);
	CHECK(rig.scheduler->spec(0).days == Weekdays::kAll);
}

TEST_CASE("ScheduleAdjust.ImageSwapKeepsOrDropsOverrides") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);

	// The same entry, still adjustable, the value fits: kept.
	{
		ScopedCapture capture;
		Morning again;
		REQUIRE(rig.load({again.spec}) == ScheduleStatus::ok);
		CHECK(rig.scheduler->overridden(0) == ScheduleMember::kTime);
		CHECK(rig.scheduler->spec(0).time.hour == 6);
		CHECK_FALSE(capture.sink.contains("dropped"));
	}
	// Bounds narrowed so that 06:00 no longer fits: dropped with a log line.
	{
		ScopedCapture capture;
		Morning narrow(6 * 3600 + 900, 9 * 3600);
		REQUIRE(rig.load({narrow.spec}) == ScheduleStatus::ok);
		CHECK(rig.scheduler->overridden(0) == 0);
		CHECK(rig.scheduler->spec(0).time.minute == 30);
		CHECK(capture.sink.contains(
			"W hc.sched: schedule morning: override of time (06:00:00) dropped: it no longer fits the image's bounds"));
	}
	// `adjustable` no longer lists time: dropped.
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);
	{
		ScopedCapture capture;
		Morning days_only(5 * 3600, 9 * 3600, ScheduleMember::kDays);
		REQUIRE(rig.load({days_only.spec}) == ScheduleStatus::ok);
		CHECK(rig.scheduler->overridden(0) == 0);
		CHECK(capture.sink.contains(
			"schedule morning: override of time (06:00:00) dropped: the image no longer declares it adjustable"));
	}
	// The entry gone: the record goes with it.
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);
	REQUIRE(rig.load({weekly("other", kWorkdays, at(7, 0))}) == ScheduleStatus::ok);
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == 0);
	// A new image whose value equals the override: no override any more.
	REQUIRE(set_time(rig, 6, 0) == ScheduleChange::ok);
	Morning moved;
	moved.spec.time = at(6, 0);
	REQUIRE(rig.load({moved.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == 0);
	CHECK(rig.scheduler->spec(0).time.hour == 6);
}

TEST_CASE("ScheduleAdjust.SpringForwardOverride") {
	// An override of 02:30 on the spring-forward day fires at 03:00 CEST (S1).
	Rig rig;
	rig.time.set_utc(cet(2026, 3, 28, 12, 0)); // Saturday; Sunday 29 March is the switch
	ScheduleBound bound;
	bound.member = ScheduleMember::kTime;
	bound.min = 0;
	bound.max = 86399;
	bound.step = 1800;
	ScheduleSpec night = weekly("night", Weekdays::kAll, at(1, 0));
	night.adjustable = ScheduleMember::kTime;
	night.bounds = &bound;
	night.bound_count = 1;
	REQUIRE(rig.load({night}) == ScheduleStatus::ok);
	ScheduleSpec values;
	values.time = at(2, 30);
	REQUIRE(rig.scheduler->set_members("night", ScheduleMember::kTime, values) == ScheduleChange::ok);
	rig.run_until(cest(2026, 3, 30, 4, 0));
	CHECK(rig.take() == Firings{{"night", cest(2026, 3, 29, 3, 0)}, {"night", cest(2026, 3, 30, 2, 30)}});
}

TEST_CASE("ScheduleAdjust.ResetReturnsToTheImage") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	ScheduleSpec values;
	values.days = 0x01;
	values.time = at(6, 0);
	REQUIRE(rig.scheduler->set_members("morning", ScheduleMember::kDays | ScheduleMember::kTime, values) ==
		ScheduleChange::ok);
	REQUIRE(rig.scheduler->set_enabled("morning", false));
	REQUIRE(rig.scheduler->reset_overrides("morning"));
	CHECK(rig.scheduler->overridden(0) == 0);
	CHECK(rig.scheduler->enabled(0));
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 6, 30));
	CHECK_FALSE(rig.scheduler->reset_overrides("nope"));
}

TEST_CASE("ScheduleAdjust.OtherKinds") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	ScheduleSpec pump = interval("pump", Weekdays::kAll, 15, at(8, 0), at(18, 0));
	pump.adjustable = ScheduleMember::kEvery | ScheduleMember::kFrom | ScheduleMember::kUntil;
	ScheduleSpec party = once("party", Date{2026, 12, 31}, at(23, 0));
	party.adjustable = ScheduleMember::kAt;
	ScheduleBound offset_bound;
	offset_bound.member = ScheduleMember::kOffset;
	offset_bound.min = -3600;
	offset_bound.max = 3600;
	offset_bound.step = 300;
	ScheduleSpec blinds = astro("blinds", SunEvent::sunset, -1800);
	blinds.adjustable = ScheduleMember::kOffset | ScheduleMember::kNotAfter;
	blinds.has_not_after = true;
	blinds.not_after = at(22, 0);
	blinds.bounds = &offset_bound;
	blinds.bound_count = 1;
	REQUIRE(rig.load({pump, party, blinds}) == ScheduleStatus::ok);

	ScheduleSpec values;
	values.from = at(19, 0);
	{
		ScopedCapture capture;
		CHECK(rig.scheduler->set_members("pump", ScheduleMember::kFrom, values) == ScheduleChange::malformed);
		CHECK(capture.sink.contains("from would be later than until"));
	}
	values.from = at(9, 0);
	values.until = at(19, 0);
	values.every_minutes = 30;
	REQUIRE(rig.scheduler->set_members("pump", ScheduleMember::kFrom | ScheduleMember::kUntil | ScheduleMember::kEvery,
		values) == ScheduleChange::ok);
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 9, 0));
	values.at_date = Date{2027, 1, 1};
	values.at_time = at(0, 30);
	REQUIRE(rig.scheduler->set_members("party", ScheduleMember::kAt, values) == ScheduleChange::ok);
	CHECK(rig.scheduler->next_firing(1) == cet(2027, 1, 1, 0, 30));
	values.offset_s = -2000;
	CHECK(rig.scheduler->set_members("blinds", ScheduleMember::kOffset, values) == ScheduleChange::out_of_bounds);
	values.offset_s = -3600;
	values.not_after = at(21, 30);
	REQUIRE(rig.scheduler->set_members("blinds", ScheduleMember::kOffset | ScheduleMember::kNotAfter, values) ==
		ScheduleChange::ok);

	// All of them in the records, restored after a restart.
	rig.boot();
	REQUIRE(rig.load({pump, party, blinds}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == (ScheduleMember::kEvery | ScheduleMember::kFrom | ScheduleMember::kUntil));
	CHECK(rig.scheduler->spec(0).every_minutes == 30);
	CHECK(rig.scheduler->overridden(1) == ScheduleMember::kAt);
	CHECK(rig.scheduler->spec(1).at_date == (Date{2027, 1, 1}));
	CHECK(rig.scheduler->overridden(2) == (ScheduleMember::kOffset | ScheduleMember::kNotAfter));
	CHECK(rig.scheduler->spec(2).offset_s == -3600);
	CHECK(rig.scheduler->spec(2).not_after.minute == 30);

	// A new image whose `until` the kept `from` would pass: the contradicting override goes.
	ScopedCapture capture;
	ScheduleSpec short_pump = interval("pump", Weekdays::kAll, 15, at(8, 0), at(8, 30));
	short_pump.adjustable = ScheduleMember::kEvery | ScheduleMember::kFrom;
	REQUIRE(rig.load({short_pump, party, blinds}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kEvery);
	CHECK(capture.sink.contains("override of from (09:00:00) dropped: it contradicts the entry's other values"));
	CHECK(capture.sink.contains("override of until (19:00:00) dropped: the image no longer declares it adjustable"));
}

TEST_CASE("ScheduleAdjust.OnceThatFiredStaysDone") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	ScheduleSpec party = once("party", Date{2026, 10, 12}, at(6, 0));
	party.adjustable = ScheduleMember::kAt;
	REQUIRE(rig.load({party}) == ScheduleStatus::ok);
	rig.run_until(cest(2026, 10, 12, 7, 0));
	CHECK(rig.take().size() == 1);
	ScheduleSpec values;
	values.at_date = Date{2026, 10, 13};
	values.at_time = at(6, 0);
	REQUIRE(rig.scheduler->set_members("party", ScheduleMember::kAt, values) == ScheduleChange::ok);
	rig.run_until(cest(2026, 10, 14, 7, 0));
	CHECK(rig.take().empty());
}

TEST_CASE("ScheduleAdjust.LoadRefusesBadDeclarations") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	ScopedCapture capture(hc::LogLevel::error);
	auto refused = [&](const ScheduleSpec& spec, const char* reason) {
		capture.sink.lines.clear();
		CHECK(rig.load({spec}) == ScheduleStatus::malformed);
		CHECK_MESSAGE(capture.sink.contains(reason), (capture.sink.lines.empty() ? std::string() : capture.sink.lines.back()));
	};
	ScheduleSpec yearly_days = yearly("y", 12, 24, at(17, 0));
	yearly_days.adjustable = ScheduleMember::kDays;
	refused(yearly_days, "adjustable names a member this kind cannot adjust");
	ScheduleSpec astro_nb = astro("a", SunEvent::sunset, 0);
	astro_nb.adjustable = ScheduleMember::kNotBefore;
	refused(astro_nb, "notBefore is adjustable, but the entry sets none");
	Morning outside(7 * 3600, 9 * 3600);
	refused(outside.spec, "the image's value lies outside its bounds");
	Morning off_step;
	off_step.spec.time = at(6, 32);
	refused(off_step.spec, "the image's value is not on its step");
	Morning inverted(9 * 3600, 5 * 3600);
	refused(inverted.spec, "min is greater than its max");
	Morning not_adjustable(5 * 3600, 9 * 3600, ScheduleMember::kDays);
	not_adjustable.spec.bounds = &not_adjustable.bound;
	not_adjustable.spec.bound_count = 1;
	refused(not_adjustable.spec, "bounds name a member that is not adjustable");
	Morning days_bound;
	days_bound.bound.member = ScheduleMember::kDays;
	days_bound.bound.min = 1;
	days_bound.bound.max = 127;
	days_bound.bound.step = 0;
	refused(days_bound.spec, "days take no bounds");
}

TEST_CASE("ScheduleAdjust.TimeDatapoint") {
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	const hc::DatapointSpec dps[] = {{"schedule/morning/time", hc::ValueType::time_of_day,
		hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit}};
	REQUIRE(rig.model.load(dps, 1) == hc::DatapointStatus::ok);
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	const hc::Datapoint* dp = rig.model.get(0);
	REQUIRE(dp->technology() == rig.scheduler.get());
	REQUIRE(dp->quality() == hc::Quality::good);
	CHECK(dp->value().as_time().hour == 6);
	CHECK(dp->value().as_time().minute == 30);

	// A command is a PUT: bounded, persisted, effective.
	hc::TimeOfDay six;
	six.hour = 6;
	CHECK(rig.model.command(0, hc::Value::of_time(six)) == hc::DatapointStatus::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kTime);
	rig.loop.run_once(rig.time.mono_ms); // the evaluation follows from the loop
	CHECK(rig.scheduler->next_firing(0) == cest(2026, 10, 12, 6, 0));
	{
		ScopedCapture capture;
		hc::TimeOfDay early;
		early.hour = 4;
		CHECK(rig.model.command(0, hc::Value::of_time(early)) == hc::DatapointStatus::refused);
		hc::TimeOfDay monday = six;
		monday.day_of_week = 1;
		CHECK(rig.model.command(0, hc::Value::of_time(monday)) == hc::DatapointStatus::refused);
	}
	CHECK(dp->value().as_time().hour == 6);
	// The API's change reports the datapoint.
	REQUIRE(set_time(rig, 7, 15) == ScheduleChange::ok);
	CHECK(dp->value().as_time().hour == 7);
	CHECK(dp->value().as_time().minute == 15);
	REQUIRE(rig.scheduler->reset_overrides("morning"));
	CHECK(dp->value().as_time().minute == 30);
	rig.run_until(cest(2026, 10, 12, 7, 0));
	CHECK(rig.take() == Firings{{"morning", cest(2026, 10, 12, 6, 30)}});
}

TEST_CASE("ScheduleAdjust.RecordFormats") {
	// The persisted record (doc/time.md): version 1 without member overrides, as before #58;
	// version 2 with `<member>=<scalar>` between the flags and the id.
	Rig rig;
	rig.time.set_utc(cest(2026, 10, 12, 5, 0));
	char key[Scheduler::kRecordKeySize];
	Scheduler::record_key("morning", key);
	int64_t fired = cest(2026, 10, 9, 6, 30);
	std::string v1 = "1;" + std::to_string(fired) + ";" + std::to_string(fired) + ";0;morning";
	REQUIRE(rig.settings.set_string(key, v1.c_str()) == hc::SettingsStatus::ok);
	Morning morning;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK_FALSE(rig.scheduler->enabled(0)); // the enable override of version 1
	CHECK(rig.scheduler->overridden(0) == 0);
	CHECK(rig.scheduler->last_fired(0) == fired);

	std::string v2 = "2;" + std::to_string(fired) + ";" + std::to_string(fired) + ";;time=21600,days=15;morning";
	REQUIRE(rig.settings.set_string(key, v2.c_str()) == hc::SettingsStatus::ok);
	rig.boot();
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->enabled(0));
	CHECK(rig.scheduler->overridden(0) == (ScheduleMember::kTime | ScheduleMember::kDays));
	CHECK(rig.scheduler->spec(0).time.hour == 6);
	CHECK(rig.scheduler->spec(0).time.minute == 0);
	CHECK(rig.scheduler->spec(0).days == 0x0F);
	char record[128];
	REQUIRE(rig.settings.get_string(key, record, sizeof(record)) == hc::SettingsStatus::ok);
	CHECK(std::string(record).find(";time=21600,days=15;morning") != std::string::npos);

	// An unreadable override is dropped with a line, the others stay.
	std::string bad = "2;;;;time=x,warp=3,days=15;morning";
	REQUIRE(rig.settings.set_string(key, bad.c_str()) == hc::SettingsStatus::ok);
	rig.boot();
	ScopedCapture capture;
	REQUIRE(rig.load({morning.spec}) == ScheduleStatus::ok);
	CHECK(rig.scheduler->overridden(0) == ScheduleMember::kDays);
	CHECK(capture.sink.count("dropped: unreadable") == 2);
}
