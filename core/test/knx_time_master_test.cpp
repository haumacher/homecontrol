/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The KNX time master on the real time service, the datapoint model, the KNX adapter and the tunnel
// to a fake gateway, under the fake clock with central European time.

#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/knx/adapter.h"
#include "hc/knx/time_master.h"
#include "hc/time_service.h"
#include "hc/tz.h"
#include "knx_test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::DateTime;
using hc::Dpt;
using hc::LocalTime;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::knx::GroupAddress;
using hc::knx::KnxAdapter;
using hc::knx::KnxMapping;
using hc::knx::TimeMaster;
using hc::knx::TimeMasterConfig;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;

namespace {

int64_t utc(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return hc::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}
int64_t cet(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return utc(year, month, day, hour, minute, second) - 3600;
}
int64_t cest(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
	return utc(year, month, day, hour, minute, second) - 7200;
}

/// The EU switch of `year` in March or October: the last Sunday, 01:00 UTC.
int64_t eu_switch(int year, unsigned month) {
	int64_t days = hc::days_from_civil(year, month, 31);
	days -= hc::weekday_from_days(days) % 7;
	return days * 86400 + 3600;
}

/// The wall clock follows the fake monotonic clock in whole seconds; SNTP is the flag.
class WallClock : public hc::Clock {
public:
	explicit WallClock(const hc_test::FakeClock& clock) : clock_(clock) {}

	uint64_t monotonic_ms() const override { return clock_.now_ms; }
	hc::WallTime wall_time() const override { return hc::WallTime{now(), synchronized}; }

	int64_t now() const { return base + static_cast<int64_t>(clock_.now_ms / 1000); }
	/// Sets the wall clock to `t` (a step if it differs from the reading).
	void set(int64_t t) { base = t - static_cast<int64_t>(clock_.now_ms / 1000); }

	int64_t base = 0;
	bool synchronized = false;

private:
	const hc_test::FakeClock& clock_;
};

/// Central European time with the EU rules.
class CetZone : public hc::TimeZone {
public:
	void apply(const char*) override {}

	bool to_local(int64_t t, LocalTime& out) const override {
		int year = hc::date_from_days(t / 86400).year;
		bool dst = t >= eu_switch(year, 3) && t < eu_switch(year, 10);
		int64_t local = t + (dst ? 7200 : 3600);
		int64_t days = local / 86400;
		int64_t rest = local - days * 86400;
		hc::Date date = hc::date_from_days(days);
		out.year = date.year;
		out.month = date.month;
		out.day = date.day;
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.dst = dst;
		std::snprintf(out.zone, sizeof(out.zone), "%s", dst ? "CEST" : "CET");
		return true;
	}
};

constexpr uint8_t kRT = DatapointFlags::kReadable | DatapointFlags::kTransmit;

/// A group telegram the controller sent: wall time and "dest len tpdu" as hex.
struct Telegram {
	int64_t utc;
	std::string text;
};

/// Ids: 0 knx.time, 1 knx.date, 2 knx.datetime (the defaults, R+T), 3 light (the adapter's, sends
/// 1/0/1), 4 hall.clock (time of day, R+W+T), 5 plain.time (no T), 6 other.time (the adapter's,
/// sends 0/0/9). The image's mappings: light and other.time.
struct TimeFixture : hc_test::TunnelFixture {
	WallClock wall{clock};
	CetZone zone;
	hc_test::FakeSettings settings;
	hc::TimeService service{loop, wall, zone, settings};
	hc::DatapointModel model{clock};
	KnxAdapter adapter{loop, clock, model, tunnel};
	TimeMaster master{loop, service, model, adapter};
	std::vector<Telegram> telegrams;
	size_t served = 0;
	uint8_t gateway_sequence = 0;

	static constexpr uint16_t kTimeGa = 0x0001;     // 0/0/1
	static constexpr uint16_t kDateGa = 0x0002;     // 0/0/2
	static constexpr uint16_t kDateTimeGa = 0x0003; // 0/0/3

	TimeFixture() {
		DatapointSpec specs[] = {
			{"knx.time", ValueType::time_of_day, kRT},
			{"knx.date", ValueType::date, kRT},
			{"knx.datetime", ValueType::date_time, kRT},
			{"light", ValueType::boolean, kRT | DatapointFlags::kWritable},
			{"hall.clock", ValueType::time_of_day, kRT | DatapointFlags::kWritable},
			{"plain.time", ValueType::time_of_day, DatapointFlags::kReadable},
			{"other.time", ValueType::time_of_day, kRT},
		};
		REQUIRE(model.load(specs, 7) == DatapointStatus::ok);
		adapter.start();
		KnxMapping image[2];
		image_mappings(image);
		REQUIRE(adapter.configure(image, 2));
		REQUIRE(service.add_listener(master, TimeMaster::kTimeEvents));
	}

	static void image_mappings(KnxMapping* out) {
		out[0] = KnxMapping();
		out[0].datapoint = 3;
		out[0].dpt = Dpt{1, 1};
		out[0].has_send = true;
		out[0].send = GroupAddress::of(1, 0, 1);
		out[1] = KnxMapping();
		out[1].datapoint = 6;
		out[1].dpt = Dpt{10, 1};
		out[1].has_send = true;
		out[1].send = GroupAddress::of(0, 0, 9);
	}

	static TimeMasterConfig all(uint32_t interval_s = TimeMasterConfig::kDefaultIntervalS) {
		TimeMasterConfig config;
		config.time.enabled = true;
		config.time.address = GroupAddress{kTimeGa};
		config.date.enabled = true;
		config.date.address = GroupAddress{kDateGa};
		config.date_time.enabled = true;
		config.date_time.address = GroupAddress{kDateTimeGa};
		config.interval_s = interval_s;
		return config;
	}

	/// What the image loader does: the time master's configuration, then the merged mapping.
	void setup(const TimeMasterConfig& config) {
		REQUIRE(master.configure(config));
		KnxMapping merged[2 + TimeMaster::kMaxOutputs];
		image_mappings(merged);
		size_t n = 2 + master.mappings(merged + 2, TimeMaster::kMaxOutputs);
		REQUIRE(adapter.configure(merged, n));
	}

	/// The time service starts with the wall clock at `t`, the tunnel comes up.
	void boot(int64_t t, bool synchronized) {
		wall.set(t);
		wall.synchronized = synchronized;
		service.start();
		connect(7);
		served = socket.sent.size();
	}

	/// The fake gateway: answers heartbeats, acknowledges and confirms every telegram, records them.
	void serve() {
		while (served < socket.sent.size()) {
			Bytes d = socket.sent[served++].data;
			uint16_t service_type = static_cast<uint16_t>(d[2] << 8 | d[3]);
			if (service_type == 0x0207) {
				socket.inject(hc_test::connectionstate_response(7));
			} else if (service_type == 0x0420) {
				telegrams.push_back(Telegram{wall.now(), hex(Bytes(d.begin() + 16, d.end()))});
				socket.inject(hc_test::tunneling_ack(7, d[8]));
				Bytes cemi(d.begin() + 10, d.end());
				cemi[0] = 0x2E;
				cemi[4] = 0x11;
				cemi[5] = 0xFA;
				socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
			}
		}
	}

	/// Runs the loop with the gateway until the wall clock reads `t`.
	void run_until_utc(int64_t t) {
		uint64_t until = clock.now_ms + static_cast<uint64_t>((t - wall.now()) * 1000);
		for (;;) {
			loop.run_once(clock.now_ms);
			serve();
			uint64_t deadline = loop.next_deadline();
			if (deadline > until) {
				clock.now_ms = until;
				loop.run_once(clock.now_ms);
				serve();
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	/// An L_Data.ind from 1.1.10 with the TPDU `tpdu` to `destination`.
	void receive(uint16_t destination, const char* tpdu) {
		Bytes cemi = bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
		serve();
	}

	/// The group writes to `ga` sent so far.
	std::vector<Telegram> writes(uint16_t ga) const {
		char prefix[8];
		std::snprintf(prefix, sizeof(prefix), "%02X %02X", ga >> 8, ga & 0xFF);
		std::vector<Telegram> result;
		for (const Telegram& t : telegrams) {
			if (t.text.compare(0, 5, prefix) == 0 && t.text.compare(12, 2, "80") == 0) {
				result.push_back(t);
			}
		}
		return result;
	}

	size_t all_writes() const {
		size_t n = 0;
		for (const Telegram& t : telegrams) {
			n += t.text.compare(12, 2, "80") == 0 ? 1 : 0;
		}
		return n;
	}

	const hc::Datapoint& dp(uint16_t id) const { return *model.get(id); }
};

/// The DPT 19 octets of a date-time write.
Bytes date_time_octets(const Telegram& t) {
	Bytes b = bytes(t.text.c_str());
	return Bytes(b.begin() + 5, b.end());
}

std::string times(const std::vector<Telegram>& list) {
	std::string text;
	for (const Telegram& t : list) {
		text += std::to_string(t.utc) + " ";
	}
	return text;
}

} // namespace

TEST_CASE("TimeMaster.NothingWhileUnsynchronized") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 3, 27), false);
	f.setup(TimeFixture::all());
	f.run_until_utc(cest(2026, 10, 14, 12, 35, 0));
	CHECK(f.all_writes() == 0);
	CHECK_FALSE(f.master.broadcasting());
	CHECK(f.dp(0).quality() == Quality::unknown);
	f.receive(TimeFixture::kDateTimeGa, "00 00");
	CHECK(f.telegrams.empty());
	CHECK(f.capture.sink.contains("not answering the read of 0/0/3: 'knx.datetime' has no good value (unknown)"));
}

TEST_CASE("TimeMaster.FirstBroadcastOnSynchronizationThenAligned") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 3, 27), false);
	f.setup(TimeFixture::all());
	f.run_until_utc(cest(2026, 10, 14, 12, 4, 0));
	// SNTP: the first fix steps the clock and sets the flag.
	f.wall.set(cest(2026, 10, 14, 12, 4, 30));
	f.wall.synchronized = true;
	f.run_until_utc(cest(2026, 10, 14, 12, 4, 32));
	REQUIRE(f.all_writes() == 3);
	CHECK(f.writes(TimeFixture::kTimeGa).at(0).utc <= cest(2026, 10, 14, 12, 4, 31));
	CHECK(f.dp(0).value().as_time().minute == 4);
	CHECK(f.master.next_tick_utc() == cest(2026, 10, 14, 12, 10, 0));
	CHECK(f.capture.sink.contains("I hc.knx: time master: broadcasting the time on 3 addresses every 600 s"));

	f.run_until_utc(cest(2026, 10, 14, 12, 20, 5));
	std::vector<Telegram> time = f.writes(TimeFixture::kTimeGa);
	std::vector<Telegram> date = f.writes(TimeFixture::kDateGa);
	std::vector<Telegram> date_time = f.writes(TimeFixture::kDateTimeGa);
	REQUIRE(time.size() == 3);
	REQUIRE(date.size() == 3);
	REQUIRE(date_time.size() == 3);
	// Aligned to the wall clock: 12:10:00 and 12:20:00 sharp, Wednesday 2026-10-14, summer time.
	CHECK(time[1].utc == cest(2026, 10, 14, 12, 10, 0));
	CHECK(time[2].utc == cest(2026, 10, 14, 12, 20, 0));
	CHECK(time[1].text == "00 01 04 00 80 6C 0A 00");
	CHECK(date[1].text == "00 02 04 00 80 0E 0A 1A");
	// DPT 19: year 126, month, day, Wednesday 12 h, 10 min, 0 s, SUTI|NWD, CLQ|SRC.
	CHECK(date_time[1].text == "00 03 09 00 80 7E 0A 0E 6C 0A 00 21 C0");
	CHECK(time[2].text == "00 01 04 00 80 6C 14 00");
	// The datapoints show what was sent.
	CHECK(f.dp(0).value().as_time() == hc::TimeOfDay{3, 12, 20, 0});
	CHECK(f.dp(1).value().as_date() == hc::Date{2026, 10, 14});
	DateTime dt = f.dp(2).value().as_date_time();
	CHECK(dt.flags == (DateTime::kSummerTime | DateTime::kNoWorkingDay | DateTime::kClockQuality | DateTime::kSyncReliable));
	CHECK(f.dp(2).quality() == Quality::good);
	CHECK(f.master.last_broadcast_utc() == cest(2026, 10, 14, 12, 20, 0));
	CHECK_FALSE(f.capture.sink.contains("refus"));
	CHECK_FALSE(f.capture.sink.contains("E hc."));
}

TEST_CASE("TimeMaster.WinterFlagsAndConfigureWhileSynchronized") {
	TimeFixture f;
	f.boot(cet(2026, 12, 2, 12, 9, 50), true);
	f.setup(TimeFixture::all());
	CHECK(f.all_writes() == 0); // not from within configure(): the adapter is configured after it
	f.run_until_utc(cet(2026, 12, 2, 12, 9, 51));
	REQUIRE(f.all_writes() == 3);
	CHECK(f.writes(TimeFixture::kTimeGa).at(0).text == "00 01 04 00 80 6C 09 32");
	f.run_until_utc(cet(2026, 12, 2, 12, 10, 1));
	REQUIRE(f.all_writes() == 6);
	CHECK(f.writes(TimeFixture::kTimeGa).at(1).text == "00 01 04 00 80 6C 0A 00");
	CHECK(f.writes(TimeFixture::kDateGa).at(1).text == "00 02 04 00 80 02 0C 1A");
	// Winter: no SUTI.
	CHECK(f.writes(TimeFixture::kDateTimeGa).at(1).text == "00 03 09 00 80 7E 0C 02 6C 0A 00 20 C0");
}

TEST_CASE("TimeMaster.ReadAnsweredFromTheLastSentValue") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 9, 0), true);
	f.setup(TimeFixture::all());
	f.run_until_utc(cest(2026, 10, 14, 12, 13, 0));
	size_t before = f.telegrams.size();
	f.receive(TimeFixture::kDateTimeGa, "00 00");
	REQUIRE(f.telegrams.size() == before + 1);
	// The value of 12:10:00, not the time of the read.
	CHECK(f.telegrams.back().text == "00 03 09 00 40 7E 0A 0E 6C 0A 00 21 C0");
	f.receive(TimeFixture::kTimeGa, "00 00");
	CHECK(f.telegrams.back().text == "00 01 04 00 40 6C 0A 00");
	// Another device writing the address does not change what the time master sent.
	f.receive(TimeFixture::kTimeGa, "00 80 6D 00 00");
	CHECK(f.dp(0).value().as_time() == hc::TimeOfDay{3, 12, 10, 0});
}

TEST_CASE("TimeMaster.SynchronizationLostTurnsStale") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 9, 0), true);
	f.setup(TimeFixture::all());
	f.run_until_utc(cest(2026, 10, 14, 12, 12, 0));
	size_t sent = f.all_writes();
	REQUIRE(sent == 6);
	f.wall.synchronized = false;
	f.run_until_utc(cest(2026, 10, 14, 12, 12, 2));
	CHECK(f.capture.sink.contains("W hc.knx: time master: the wall clock lost synchronization; the time broadcasts stop"));
	CHECK(f.dp(0).quality() == Quality::stale);
	CHECK(f.dp(1).quality() == Quality::stale);
	CHECK(f.dp(2).quality() == Quality::stale);
	CHECK(f.dp(0).value().as_time() == hc::TimeOfDay{3, 12, 10, 0});
	CHECK_FALSE(f.master.broadcasting());
	f.run_until_utc(cest(2026, 10, 14, 12, 45, 0));
	CHECK(f.all_writes() == sent);
	size_t before = f.telegrams.size();
	f.receive(TimeFixture::kTimeGa, "00 00");
	CHECK(f.telegrams.size() == before);
	CHECK(f.capture.sink.contains("not answering the read of 0/0/1: 'knx.time' has no good value (stale)"));
	// Synchronized again: broadcasts at once and on the grid.
	f.wall.synchronized = true;
	f.run_until_utc(cest(2026, 10, 14, 12, 50, 0));
	CHECK(f.all_writes() == sent + 6);
	CHECK(f.dp(0).quality() == Quality::good);
}

TEST_CASE("TimeMaster.ClockJumpRealigns") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 3, 0), true);
	TimeMasterConfig config = TimeFixture::all();
	config.date.enabled = false;
	config.date_time.enabled = false;
	f.setup(config);
	f.run_until_utc(cest(2026, 10, 14, 12, 5, 0));
	REQUIRE(f.writes(TimeFixture::kTimeGa).size() == 1);
	f.wall.set(cest(2026, 10, 14, 12, 47, 30)); // SNTP corrects by 42 minutes
	f.run_until_utc(cest(2026, 10, 14, 13, 0, 5));
	std::vector<Telegram> time = f.writes(TimeFixture::kTimeGa);
	REQUIRE(time.size() == 4);
	CHECK(time[1].utc <= cest(2026, 10, 14, 12, 47, 31)); // the jump: at once
	CHECK(time[2].utc == cest(2026, 10, 14, 12, 50, 0));  // then on the grid again
	CHECK(time[3].utc == cest(2026, 10, 14, 13, 0, 0));
}

TEST_CASE("TimeMaster.SpringForwardSendsTheNewLocalTime") {
	TimeFixture f;
	f.boot(cet(2026, 3, 29, 0, 30, 0), true);
	f.setup(TimeFixture::all(3600));
	f.run_until_utc(cest(2026, 3, 29, 4, 0, 5));
	std::vector<Telegram> time = f.writes(TimeFixture::kTimeGa);
	std::vector<Telegram> date_time = f.writes(TimeFixture::kDateTimeGa);
	INFO(times(time));
	REQUIRE(time.size() == 4); // 00:30 (configured), 01:00 CET, 03:00 CEST, 04:00 CEST
	CHECK(time[1].text == "00 01 04 00 80 E1 00 00"); // Sunday 01:00:00
	CHECK(time[2].utc == eu_switch(2026, 3));
	CHECK(time[2].text == "00 01 04 00 80 E3 00 00"); // Sunday 03:00:00: the new local time
	CHECK(date_time[1].text == "00 03 09 00 80 7E 03 1D E1 00 00 20 C0");
	CHECK(date_time[2].text == "00 03 09 00 80 7E 03 1D E3 00 00 21 C0"); // and SUTI
	CHECK(time[3].utc == cest(2026, 3, 29, 4, 0, 0));
}

TEST_CASE("TimeMaster.MidnightOnceWhenTheIntervalHitsIt") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 23, 30, 0), true);
	TimeMasterConfig config = TimeFixture::all(3600);
	f.setup(config);
	f.run_until_utc(cest(2026, 10, 15, 0, 30, 0));
	std::vector<Telegram> time = f.writes(TimeFixture::kTimeGa);
	INFO(times(time));
	REQUIRE(time.size() == 2); // 23:30 (configured) and 00:00 on the grid; the day change adds none
	CHECK(time[1].utc == cest(2026, 10, 15, 0, 0, 0));
	CHECK(time[1].text == "00 01 04 00 80 80 00 00"); // Thursday 00:00:00
}

TEST_CASE("TimeMaster.MidnightBroadcastAcrossTheFallBack") {
	for (bool at_midnight : {true, false}) {
		CAPTURE(at_midnight);
		TimeFixture f;
		f.boot(cest(2026, 10, 24, 23, 0, 0), true);
		TimeMasterConfig config = TimeFixture::all(7 * 3600); // the grid does not hit midnight
		config.at_midnight = at_midnight;
		f.setup(config);
		f.run_until_utc(cet(2026, 10, 26, 0, 0, 5));
		std::vector<Telegram> date_time = f.writes(TimeFixture::kDateTimeGa);
		INFO(times(date_time));
		std::vector<Bytes> midnights;
		bool offset_broadcast = false;
		for (const Telegram& t : date_time) {
			Bytes o = date_time_octets(t);
			if ((o[3] & 0x1F) == 0 && o[4] == 0 && o[5] <= 1) {
				midnights.push_back(o);
			}
			if (t.utc >= eu_switch(2026, 10) && t.utc <= eu_switch(2026, 10) + 1) {
				offset_broadcast = true;
				CHECK((o[3] & 0x1F) == 2); // 02:00 CET, the new local time
				CHECK(o[6] == 0x20);       // winter
			}
		}
		CHECK(offset_broadcast);
		if (!at_midnight) {
			CHECK(midnights.empty());
			continue;
		}
		REQUIRE(midnights.size() == 2);
		// Sunday 25 October 00:00 CEST: summer time; Monday 26 October 00:00 CET: winter.
		CHECK(midnights[0][2] == 25);
		CHECK((midnights[0][3] >> 5) == 7);
		CHECK(midnights[0][6] == 0x21);
		CHECK(midnights[1][2] == 26);
		CHECK((midnights[1][3] >> 5) == 1);
		CHECK(midnights[1][6] == 0x20);
		CHECK(midnights[1][7] == 0xC0);
	}
}

TEST_CASE("TimeMaster.ConfigurationRefusals") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 0, 0), false);
	f.setup(TimeFixture::all());
	REQUIRE(f.master.size() == 3);
	const char* error = nullptr;
	auto refused = [&](const TimeMasterConfig& config, const char* text) {
		CHECK_FALSE(f.master.configure(config, &error));
		CHECK(f.capture.sink.contains(text));
		CHECK(f.master.size() == 3); // the previous configuration stays
	};
	TimeMasterConfig c = TimeFixture::all(59);
	refused(c, "E hc.knx: refusing the time master configuration: interval 59 s outside 60..86400 s");
	c = TimeFixture::all(86401);
	refused(c, "interval 86401 s outside 60..86400 s");
	c = TimeFixture::all();
	c.date.address = c.time.address;
	refused(c, "the address 0/0/1 is configured twice");
	c = TimeFixture::all();
	c.time.address = GroupAddress::of(1, 0, 1);
	refused(c, "10.001 on 1/0/1: the address is the sending address of 'light'");
	c = TimeFixture::all();
	c.date.datapoint = "nope";
	refused(c, "11.001 on 0/0/2: no datapoint 'nope' in the model");
	c = TimeFixture::all();
	c.time.datapoint = "light";
	refused(c, "10.001 on 0/0/1: 'light' is a boolean datapoint, the DPT needs time of day");
	c = TimeFixture::all();
	c.time.datapoint = "plain.time";
	refused(c, "'plain.time' does not transmit (no T flag)");
	c = TimeFixture::all();
	c.time.datapoint = "other.time";
	refused(c, "'other.time' belongs to knx");
	CHECK(std::string(error) == "invalid time master configuration (the log names it)");
	// Nothing configured: allowed, the time master is off.
	CHECK(f.master.configure(TimeMasterConfig()));
	CHECK(f.master.size() == 0);
}

TEST_CASE("TimeMaster.OwnsItsDatapointsAndRefusesCommands") {
	TimeFixture f;
	f.boot(cest(2026, 10, 14, 12, 0, 0), true);
	TimeMasterConfig config = TimeFixture::all();
	config.time.datapoint = "hall.clock";
	f.setup(config);
	KnxMapping m[TimeMaster::kMaxOutputs];
	REQUIRE(f.master.mappings(m, TimeMaster::kMaxOutputs) == 3);
	CHECK(m[0].datapoint == 4);
	CHECK(m[0].transmit_only);
	CHECK(m[0].send == GroupAddress{TimeFixture::kTimeGa});
	CHECK((m[0].dpt == Dpt{10, 1}));
	CHECK((m[2].dpt == Dpt{19, 1}));
	CHECK(f.dp(4).technology() == &f.master);
	CHECK(f.dp(4).transmitter() == &f.adapter);
	f.run_until_utc(cest(2026, 10, 14, 12, 0, 1));
	size_t sent = f.all_writes();
	CHECK(sent == 3);
	CHECK(f.model.command(4, Value::of_time(hc::TimeOfDay{1, 1, 1, 1})) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("W hc.knx: refusing command for 'hall.clock': the time master sends the controller's clock"));
	f.serve();
	CHECK(f.all_writes() == sent);
	CHECK(f.dp(4).value().as_time() == hc::TimeOfDay{3, 12, 0, 0});
}
