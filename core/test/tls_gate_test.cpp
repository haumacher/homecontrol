/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The TLS gate (homecontrol #110/#112): one TLS session start at a time, the E-Home link first, a
// further session only with room in the DMA-capable memory; a waiting session goes on when the holder
// releases or the platform reports freed memory, never by polling.

#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/event_loop.h"
#include "hc/tls_gate.h"
#include "test_support.h"

using hc::TlsGate;

namespace {

/// The platform's DMA-capable memory, set by the test; records the notifications asked for.
class FakeMemory : public hc::MemoryWatch {
public:
	bool dma_room(hc::DmaRoom& out) const override {
		++reads;
		out = room;
		return known;
	}
	void notify_at(uint32_t free_bytes, hc::MemoryListener& l) override {
		armed_at = free_bytes;
		listener = &l;
		++requests;
	}
	void cancel_notify() override {
		listener = nullptr;
		armed_at = 0;
	}

	/// Memory was freed: the platform's notification, when one is asked for and the figure reached.
	void free_to(uint32_t free_bytes, uint32_t largest) {
		room.free_bytes = free_bytes;
		room.largest_block = largest;
		if (listener != nullptr && free_bytes >= armed_at) {
			hc::MemoryListener* l = listener;
			listener = nullptr;
			l->memory_freed();
		}
	}

	hc::DmaRoom room{60000, 24000};
	bool known = true;
	mutable int reads = 0;
	uint32_t armed_at = 0;
	int requests = 0;
	hc::MemoryListener* listener = nullptr;
};

struct Client : hc::TlsGateClient {
	void tls_gate_opened() override { ++opened; }
	int opened = 0;
};

struct Fixture {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	FakeMemory memory;
	TlsGate gate{loop, clock, &memory};

	void run() { loop.run_once(clock.now_ms); }
	void advance(uint64_t ms) {
		clock.advance(ms);
		run();
	}
};

constexpr uint32_t kNeed = TlsGate::kDeviceNeeds.session_bytes + TlsGate::kDeviceNeeds.reserve_bytes;

} // namespace

TEST_CASE("TlsGate: the device's needs are the measured figures") {
	CHECK(TlsGate::kDeviceNeeds.session_bytes == 40960);
	CHECK(TlsGate::kDeviceNeeds.reserve_bytes == 8192);
	CHECK(TlsGate::kDeviceNeeds.block_bytes == 8448);
	CHECK(kNeed == 49152);
}

TEST_CASE("TlsGate: a session is admitted with room, held until released, one at a time") {
	Fixture f;
	Client a, b;
	f.memory.room = {kNeed, TlsGate::kDeviceNeeds.block_bytes}; // exactly enough
	CHECK(f.gate.acquire(a, TlsGate::Kind::session));
	CHECK(f.gate.holds(a));
	CHECK(f.gate.acquire(a, TlsGate::Kind::session)); // asking again while holding
	// A second session waits for the first, whatever the memory says.
	CHECK_FALSE(f.gate.acquire(b, TlsGate::Kind::session));
	CHECK(f.gate.waits(b));
	CHECK_FALSE(f.gate.acquire(b, TlsGate::Kind::session)); // one place per client
	CHECK(f.gate.waiting() == 1);
	f.run();
	CHECK(b.opened == 0);
	f.gate.release(a);
	CHECK(b.opened == 0); // never from within release()
	f.run();
	CHECK(b.opened == 1);
	CHECK(f.gate.holds(b));
	CHECK(f.gate.admitted() == 2);
}

TEST_CASE("TlsGate: without room a session waits and goes on when the platform reports freed memory") {
	// The boot of 2026-11-22: HP SRAM 38120 B free beside the E-Home session and a browser.
	Fixture f;
	Client fetch;
	f.memory.room = {38120, 24064};
	CHECK_FALSE(f.gate.acquire(fetch, TlsGate::Kind::session));
	CHECK(f.gate.waits(fetch));
	CHECK(std::string(f.gate.memory_reason()) == "HP 38120 B free of 49152 B, largest block 24064 B of 8448 B");
	CHECK(f.memory.armed_at == kNeed); // a notification, no timer
	CHECK(f.loop.timer_count() == 0);
	int reads = f.memory.reads;
	f.advance(600000);
	CHECK(fetch.opened == 0);
	CHECK(f.memory.reads == reads); // no polling

	// Memory freed but not enough: no notification.
	f.memory.free_to(45000, 24064);
	f.run();
	CHECK(fetch.opened == 0);
	// Enough: admitted.
	f.memory.free_to(52000, 24064);
	f.run();
	CHECK(fetch.opened == 1);
	CHECK(f.gate.holds(fetch));
	CHECK(f.gate.waited_for_memory() == 1);
	CHECK(std::string(f.gate.memory_reason()).empty());
}

TEST_CASE("TlsGate: the E-Home link goes first and is admitted without a memory check") {
	Fixture f;
	Client fetch, other, link;
	f.memory.room = {60000, 24000};
	REQUIRE(f.gate.acquire(fetch, TlsGate::Kind::session));
	f.memory.room = {20000, 8000}; // the running fetch took it
	CHECK_FALSE(f.gate.acquire(other, TlsGate::Kind::session));
	CHECK_FALSE(f.gate.acquire(link, TlsGate::Kind::link));
	f.gate.release(fetch);
	f.run();
	CHECK(link.opened == 1); // ahead of the earlier session, though memory is short
	CHECK(other.opened == 0);
	// While the link holds the gate the session waits; after it, with room, it goes on.
	f.memory.room = {55000, 20000};
	f.gate.release(link);
	f.run();
	CHECK(other.opened == 1);

	// A link that finds the gate free is admitted at once, also with short memory.
	Fixture g;
	g.memory.room = {1000, 500};
	Client l2;
	CHECK(g.gate.acquire(l2, TlsGate::Kind::link));
}

TEST_CASE("TlsGate: a session does not pass a waiting link") {
	Fixture f;
	Client holder, link, late;
	REQUIRE(f.gate.acquire(holder, TlsGate::Kind::session));
	CHECK_FALSE(f.gate.acquire(link, TlsGate::Kind::link));
	f.gate.release(holder);
	// Before the posted evaluation: a new session finds a waiting link and waits behind it.
	CHECK_FALSE(f.gate.acquire(late, TlsGate::Kind::session));
	f.run();
	CHECK(link.opened == 1);
	CHECK(late.opened == 0);
}

TEST_CASE("TlsGate: a withdrawn wait ends the notification; a short block asks again a second later") {
	Fixture f;
	Client fetch;
	f.memory.room = {30000, 24000};
	CHECK_FALSE(f.gate.acquire(fetch, TlsGate::Kind::session));
	REQUIRE(f.memory.listener != nullptr);
	f.gate.release(fetch); // the request went (its wait limit, a cancel)
	CHECK_FALSE(f.gate.waits(fetch));
	CHECK(f.memory.listener == nullptr);

	// Free memory enough, but fragmented: the notification would fire at every free; one a second.
	f.memory.room = {52000, 4000};
	CHECK_FALSE(f.gate.acquire(fetch, TlsGate::Kind::session));
	CHECK(f.memory.listener == nullptr);
	CHECK(f.loop.timer_count() == 1);
	int requests = f.memory.requests;
	f.advance(TlsGate::kBlockRetryMs);
	CHECK(f.memory.requests == requests + 1);
	f.memory.free_to(52000, 12000);
	f.run();
	CHECK(fetch.opened == 1);
}

TEST_CASE("TlsGate: without a memory figure sessions go by the order alone") {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	TlsGate gate(loop, clock, nullptr);
	Client a, b;
	CHECK(gate.acquire(a, TlsGate::Kind::session));
	CHECK_FALSE(gate.acquire(b, TlsGate::Kind::session));
	gate.release(a);
	loop.run_once(clock.now_ms);
	CHECK(b.opened == 1);

	Fixture f;
	f.memory.known = false;
	f.memory.room = {0, 0};
	Client c;
	CHECK(f.gate.acquire(c, TlsGate::Kind::session));
}

// ---- The pause of the E-Home link (homecontrol #112) --------------------------------------------

namespace {

/// The E-Home link as the gate sees it.
struct FakeLink : hc::TlsGateLink, hc::TlsGateClient {
	bool tls_link_open() const override { return open; }
	bool tls_link_pausable() const override { return open && pausable; }
	void tls_link_pause() override {
		++pauses;
		open = false;
	}
	void tls_link_resume() override { ++resumes; }
	void tls_gate_opened() override { ++opened; }
	bool open = false;
	bool pausable = true;
	int pauses = 0;
	int resumes = 0;
	int opened = 0;
};

struct Named : Client {
	explicit Named(const char* n) : name(n) {}
	void tls_gate_describe(char* out, size_t size) const override { std::snprintf(out, size, "%s", name); }
	const char* name;
};

/// The link connects: 80000 B free before, 56000 B once its socket is open: a footprint of 24000 B.
struct PauseFixture : Fixture {
	hc_test::ScopedCapture capture;
	FakeLink link;
	PauseFixture() {
		gate.set_link(&link, &link);
		memory.room = {80000, 30000};
		REQUIRE(gate.acquire(link, TlsGate::Kind::link));
		memory.room = {56000, 28000};
		link.open = true;
		gate.release(link);
		REQUIRE(gate.link_footprint() == 24000);
	}
};

} // namespace

TEST_CASE("TlsGate pause: starts only when the fetch fits without the E-Home session") {
	// The house controller: HP 32300 B free beside the link; 32300 + 24000 >= 49152.
	PauseFixture f;
	Named language("language");
	f.memory.room = {32300, 28672};
	CHECK_FALSE(f.gate.acquire(language, TlsGate::Kind::session));
	CHECK(f.link.pauses == 1);
	CHECK(f.gate.pausing());
	CHECK(f.capture.sink.contains("I hc.tls: pausing the E-Home link for language: HP 32300 B free, the link holds "
		"24000 B, 49152 B needed"));

	// Would not fit even without the link: no pause.
	PauseFixture g;
	Named update("update");
	g.memory.room = {20000, 20000};
	CHECK_FALSE(g.gate.acquire(update, TlsGate::Kind::session));
	g.gate.link_tick();
	CHECK(g.link.pauses == 0);

	// The link is not open, or not measured: no pause.
	PauseFixture h;
	h.link.open = false;
	h.memory.room = {32300, 28672};
	Named a("a");
	CHECK_FALSE(h.gate.acquire(a, TlsGate::Kind::session));
	CHECK(h.link.pauses == 0);
	Fixture u;
	FakeLink unmeasured;
	unmeasured.open = true;
	u.gate.set_link(&unmeasured, &unmeasured);
	u.memory.room = {32300, 28672};
	Named b("b");
	CHECK_FALSE(u.gate.acquire(b, TlsGate::Kind::session));
	CHECK(unmeasured.pauses == 0);
}

TEST_CASE("TlsGate pause: a busy link is not paused; its tick starts the pause once it is idle") {
	PauseFixture f;
	f.link.pausable = false; // a deploy or a bus session runs
	f.memory.room = {32300, 28672};
	Named language("language");
	CHECK_FALSE(f.gate.acquire(language, TlsGate::Kind::session));
	CHECK(f.link.pauses == 0);
	f.gate.link_tick();
	CHECK(f.link.pauses == 0);
	f.link.pausable = true;
	f.gate.link_tick();
	CHECK(f.link.pauses == 1);
}

TEST_CASE("TlsGate pause: the waiting sessions run in one pause, one that arrives joins, the link returns at once") {
	PauseFixture f;
	Named language("language"), update("update"), external("external");
	f.memory.room = {32300, 28672};
	CHECK_FALSE(f.gate.acquire(language, TlsGate::Kind::session));
	CHECK_FALSE(f.gate.acquire(update, TlsGate::Kind::session));
	REQUIRE(f.link.pauses == 1);
	// The link's session is freed: the first runs.
	f.memory.free_to(57000, 28672);
	f.run();
	CHECK(language.opened == 1);
	CHECK(update.opened == 0);
	// One arrives during the pause: it joins.
	CHECK_FALSE(f.gate.acquire(external, TlsGate::Kind::session));
	f.advance(2000);
	f.gate.release(language);
	f.run();
	CHECK(update.opened == 1);
	CHECK(f.link.resumes == 0);
	f.gate.release(update);
	f.run();
	CHECK(external.opened == 1);
	f.gate.release(external);
	f.run();
	CHECK(f.link.resumes == 1);
	CHECK_FALSE(f.gate.pausing());
	CHECK(f.capture.sink.contains("I hc.tls: the E-Home link reconnects after a pause of 2.0 s: 3 TLS session(s) ran, "
		"nothing waits any more"));
	CHECK(f.link.pauses == 1);
	CHECK(f.loop.timer_count() == 0); // the longest pause's timer is gone
}

TEST_CASE("TlsGate pause: at most one per interval, and never longer than the longest pause") {
	PauseFixture f;
	Named language("language");
	f.memory.room = {32300, 28672};
	CHECK_FALSE(f.gate.acquire(language, TlsGate::Kind::session));
	REQUIRE(f.link.pauses == 1);
	// The memory never comes (the fetch would fail anyway): the link returns after the longest pause.
	f.advance(TlsGate::kPauseMaxMs - 1);
	CHECK(f.link.resumes == 0);
	f.advance(1);
	CHECK(f.link.resumes == 1);
	CHECK(f.gate.waits(language)); // waits for the next pause
	CHECK(f.capture.sink.contains("0 TLS session(s) ran, the longest pause is over"));

	// The link is back; its ticks do not pause it again within the interval.
	f.link.open = true;
	uint64_t since_pause = TlsGate::kPauseMaxMs;
	for (int i = 0; i < 8; ++i) {
		f.advance(60000);
		since_pause += 60000;
		f.gate.link_tick();
	}
	f.advance(TlsGate::kPauseIntervalMs - since_pause - 1);
	f.gate.link_tick();
	CHECK(f.link.pauses == 1); // 1 ms short of the interval
	f.advance(1);
	f.gate.link_tick();
	CHECK(f.link.pauses == 2);
}

TEST_CASE("TlsGate pause: with room a session is admitted directly, the link stays") {
	PauseFixture f; // no browser: room for a second session
	Named language("language");
	f.memory.room = {52000, 28672};
	CHECK(f.gate.acquire(language, TlsGate::Kind::session));
	CHECK(f.link.pauses == 0);
}
