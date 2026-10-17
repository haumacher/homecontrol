/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "test_support.h"

using hc::Connectivity;
using hc::ConnectResult;
using hc::EventLoop;
using hc::LinkState;
using hc::NetworkMode;
using hc_test::FakeClock;
using hc_test::FakeNetworkDriver;
using hc_test::ScopedCapture;

namespace {

/// A connectivity under a fake clock, driven like a platform drives the event loop.
struct Fixture {
	ScopedCapture capture;
	FakeClock clock;
	EventLoop loop;
	FakeNetworkDriver driver{clock};
	Connectivity connectivity{loop, clock, driver};

	/// Runs the loop, jumping the clock from deadline to deadline, until `until_ms`.
	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	/// Fails every attempt right after it started, as the ESP driver reports a missing access point,
	/// and runs the loop until `until_ms`.
	void fail_attempts_until(uint64_t until_ms) {
		for (;;) {
			if (connectivity.state() == LinkState::connecting) {
				connectivity.link_down("no access point");
			}
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
			loop.run_once(clock.now_ms);
		}
	}
};

struct Recorder : hc::ConnectivityListener {
	explicit Recorder(std::string name_, std::vector<std::string>& log_) : name(std::move(name_)), log(log_) {}

	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override {
		log.push_back(name + ":" + hc::link_state_name(previous) + "->" + hc::link_state_name(connectivity.state()));
	}

	std::string name;
	std::vector<std::string>& log;
};

} // namespace

TEST_CASE("Connectivity.StartConnectsAndEntersConnecting") {
	Fixture f;
	CHECK(f.connectivity.state() == LinkState::down);
	f.connectivity.start();
	CHECK(f.driver.connects == std::vector<uint64_t>{1000});
	CHECK(f.connectivity.state() == LinkState::connecting);
	CHECK(f.capture.sink.contains("I hc.net: network connecting (attempt 1)"));

	f.connectivity.link_up("192.0.2.10");
	CHECK(f.connectivity.state() == LinkState::up);
	CHECK(std::string(f.connectivity.ip()) == "192.0.2.10");
	CHECK(f.capture.sink.contains("I hc.net: network up 192.0.2.10"));
	CHECK(f.loop.timer_count() == 0); // no timeout, no retry left behind
}

TEST_CASE("Connectivity.SuccessResetsTheBackoff") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_down("auth failed");     // retry in 1 s
	f.run_until(2000);
	f.connectivity.link_down("auth failed");     // retry in 2 s
	f.run_until(4000);
	f.connectivity.link_down("auth failed");     // retry in 4 s
	f.run_until(8000);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 2000, 4000, 8000});
	CHECK(f.connectivity.failures() == 3);

	f.connectivity.link_up("192.0.2.10");
	CHECK(f.connectivity.failures() == 0);
	f.run_until(100000);
	CHECK(f.driver.connects.size() == 4); // up: nothing scheduled

	f.connectivity.link_down("beacon timeout");
	CHECK(f.connectivity.next_action_at() == 101000);
	CHECK(f.capture.sink.contains("W hc.net: network down: beacon timeout; reconnect in 1 s (failure 1)"));
}

TEST_CASE("Connectivity.DownWhileUpSchedulesAReconnect") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.clock.advance(5000);
	f.connectivity.link_down("wifi reason 200 (BEACON_TIMEOUT)");
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(std::string(f.connectivity.ip()).empty());
	CHECK(std::string(f.connectivity.reason()) == "wifi reason 200 (BEACON_TIMEOUT)");
	CHECK(f.loop.next_deadline() == 7000);
	f.run_until(7000);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 7000});
	CHECK(f.connectivity.state() == LinkState::connecting);
	f.connectivity.link_up("192.0.2.11");
	CHECK(std::string(f.connectivity.ip()) == "192.0.2.11");
}

TEST_CASE("Connectivity.RepeatedDownEventsDoNotStackRetries") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_down("first");
	f.connectivity.link_down("second");
	f.connectivity.link_down("third");
	CHECK(f.connectivity.failures() == 1);
	CHECK(f.loop.timer_count() == 1);
	CHECK(std::string(f.connectivity.reason()) == "first");
	f.run_until(10000);
	CHECK(f.driver.connects.size() == 2);
}

TEST_CASE("Connectivity.ListenersAreNotifiedInRegistrationOrder") {
	Fixture f;
	std::vector<std::string> log;
	Recorder a("a", log);
	Recorder b("b", log);
	Recorder c("c", log);
	CHECK(f.connectivity.add_listener(&a));
	CHECK(f.connectivity.add_listener(&b));
	CHECK(f.connectivity.add_listener(&c));

	f.connectivity.start();
	f.connectivity.link_connecting(); // already connecting: no change, no notification
	f.connectivity.link_up("192.0.2.10");
	f.connectivity.link_up("192.0.2.10"); // same address: no notification
	f.connectivity.link_up("192.0.2.12"); // new address: notified, up -> up
	f.connectivity.link_down("gone");
	CHECK(log == std::vector<std::string>{
		"a:down->connecting", "b:down->connecting", "c:down->connecting",
		"a:connecting->up", "b:connecting->up", "c:connecting->up",
		"a:up->up", "b:up->up", "c:up->up",
		"a:up->down", "b:up->down", "c:up->down",
	});
}

TEST_CASE("Connectivity.ListenerSeesTheNewStateAndCannotChangeIt") {
	Fixture f;
	struct Meddler : hc::ConnectivityListener {
		explicit Meddler(Connectivity& c_) : c(c_) {}
		void connectivity_changed(const Connectivity& connectivity, LinkState) override {
			seen_ip = connectivity.ip();
			c.link_down("meddling");
		}
		Connectivity& c;
		std::string seen_ip;
	} meddler(f.connectivity);
	f.connectivity.add_listener(&meddler);
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	CHECK(meddler.seen_ip == "192.0.2.10");
	CHECK(f.connectivity.state() == LinkState::up);
	CHECK(f.capture.sink.contains("E hc.net: refusing link_down from a connectivity listener"));
}

TEST_CASE("Connectivity.ListenerCapacityIsEnforced") {
	Fixture f;
	std::vector<std::string> log;
	std::vector<Recorder> recorders;
	recorders.reserve(Connectivity::kListenerCapacity + 1);
	for (size_t i = 0; i <= Connectivity::kListenerCapacity; ++i) {
		recorders.emplace_back("r", log);
	}
	for (size_t i = 0; i < Connectivity::kListenerCapacity; ++i) {
		CHECK(f.connectivity.add_listener(&recorders[i]));
	}
	CHECK_FALSE(f.connectivity.add_listener(&recorders.back()));
	CHECK(f.capture.sink.contains("E hc.net: connectivity listeners exhausted (capacity 4), refusing listener"));
	CHECK_FALSE(f.connectivity.add_listener(nullptr));
}

TEST_CASE("Connectivity.FailedConnectRetriesWithBackoff") {
	Fixture f;
	f.driver.result = ConnectResult::failed;
	f.connectivity.start();
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(f.capture.sink.contains(
		"W hc.net: network down: the driver could not start an attempt; reconnect in 1 s (failure 1)"));
	f.run_until(1000 + 1000 + 2000 + 4000);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 2000, 4000, 8000});
}

TEST_CASE("Connectivity.AttemptWithoutOutcomeTimesOut") {
	Fixture f;
	f.connectivity.start();
	f.run_until(1000 + Connectivity::kConnectTimeoutMs - 1);
	CHECK(f.connectivity.state() == LinkState::connecting);
	f.run_until(1000 + Connectivity::kConnectTimeoutMs);
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(f.capture.sink.contains("W hc.net: network down: no link after 30 s; reconnect in 1 s (failure 1)"));
	f.run_until(1000 + Connectivity::kConnectTimeoutMs + 1000);
	CHECK(f.driver.connects.size() == 2);
	CHECK(f.connectivity.state() == LinkState::connecting);
}

TEST_CASE("Connectivity.RequestConnectWhileUpReconnectsWithResetBackoff") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.clock.advance(1000);
	f.connectivity.request_connect("new credentials");
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 2000});
	CHECK(f.connectivity.state() == LinkState::connecting);
	CHECK(std::string(f.connectivity.ip()).empty());
	CHECK(f.capture.sink.contains("I hc.net: network restart requested: new credentials"));
}

TEST_CASE("Connectivity.ReportsFromWithinConnectAreRefused") {
	Fixture f;
	f.driver.on_connect = [&f]() { f.connectivity.link_up("192.0.2.10"); };
	f.connectivity.start();
	CHECK(f.capture.sink.contains(
		"E hc.net: refusing link_up from within a network driver call (report asynchronously)"));
	CHECK(f.connectivity.state() == LinkState::connecting);
}

TEST_CASE("Connectivity.InvalidAddressIsRefused") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("");
	f.connectivity.link_up(std::string(Connectivity::kMaxAddress, '1').c_str());
	CHECK(f.capture.sink.count("E hc.net: refusing link_up: address missing or longer than 45 characters") == 2);
	CHECK(f.connectivity.state() == LinkState::connecting);
}

TEST_CASE("Connectivity.SecondStartIsRefused") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.start();
	CHECK(f.driver.connects.size() == 1);
	CHECK(f.capture.sink.contains("E hc.net: refusing start: connectivity already started"));
}

TEST_CASE("Connectivity.LongReasonsAreTruncated") {
	Fixture f;
	f.connectivity.start();
	std::string reason(200, 'r');
	f.connectivity.link_down(reason.c_str());
	CHECK(std::string(f.connectivity.reason()).size() == Connectivity::kMaxReason - 1);
}

// ---- Backoff and failsafe ---------------------------------------------------------------------

TEST_CASE("Connectivity.RepeatedFailuresFollowTheBackoffSequenceUntilTheFailsafe") {
	Fixture f;
	f.connectivity.start();
	// Failures 1..6 are retried after 1, 2, 4, 8, 16, 32 s; the 7th enters the failsafe.
	f.fail_attempts_until(1000 + 63000 + Connectivity::kSettleMs);
	std::vector<uint64_t> expected{1000};
	for (uint64_t delay : {1, 2, 4, 8, 16, 32}) {
		expected.push_back(expected.back() + delay * 1000);
	}
	CHECK(f.driver.connects == expected);
	CHECK(f.connectivity.mode() == NetworkMode::failsafe_pairing);
	CHECK(f.capture.sink.contains("W hc.net: network down: no access point; reconnect in 1 s (failure 1)"));
	CHECK(f.capture.sink.contains("W hc.net: network down: no access point; reconnect in 32 s (failure 6)"));
	CHECK(f.capture.sink.contains(
		"E hc.net: stored credentials failed 7 times in a row: entering the failsafe (alternating 120 s of pairing "
		"and of retrying them; they stay stored)"));
	CHECK(f.capture.sink.contains("W hc.net: network down: no access point; pairing in 500 ms"));
	// The dongle's sequence: disconnect, settle, pair.
	CHECK(f.driver.calls.back() == "pair@64500");
	CHECK(f.driver.calls[f.driver.calls.size() - 2] == "disconnect@64000");
	CHECK(f.connectivity.pairing_active());
}

TEST_CASE("Connectivity.FailsafeAlternatesPairingAndStoredCredentials") {
	Fixture f;
	f.connectivity.start();
	f.fail_attempts_until(64500); // failsafe entered at 64000, pairing from 64500
	REQUIRE(f.driver.pairings == std::vector<uint64_t>{64500});
	size_t connects = f.driver.connects.size();

	// Pairing phase: 120 s from the failsafe entry. Then stop pairing, settle, connect.
	f.run_until(64000 + 120000 - 1);
	CHECK(f.driver.connects.size() == connects);
	f.run_until(64000 + 120000 + 500);
	CHECK(f.connectivity.mode() == NetworkMode::failsafe_connecting);
	CHECK(f.driver.pairing_stops == 1);
	CHECK(f.driver.connects.back() == 184500);
	CHECK(f.capture.sink.contains("I hc.net: failsafe: pairing phase over, retrying the stored credentials for 120 s"));

	// Credentials phase: failures are retried with the backoff, never escalate again.
	f.fail_attempts_until(64000 + 240000 - 1);
	CHECK(f.connectivity.mode() == NetworkMode::failsafe_connecting);
	CHECK(f.driver.connects.size() > connects + 5);

	// Back to pairing after the phase.
	f.fail_attempts_until(64000 + 240000 + 500);
	CHECK(f.connectivity.mode() == NetworkMode::failsafe_pairing);
	CHECK(f.driver.pairings.back() == 304500);
	CHECK(f.capture.sink.contains("I hc.net: failsafe: stored-credentials phase over, pairing for 120 s"));
}

TEST_CASE("Connectivity.LinkUpEndsTheFailsafe") {
	Fixture f;
	f.connectivity.start();
	f.fail_attempts_until(64000 + 120000);
	f.run_until(64000 + 120000 + 500); // in the credentials phase, attempt running
	REQUIRE(f.connectivity.mode() == NetworkMode::failsafe_connecting);
	REQUIRE(f.connectivity.state() == LinkState::connecting);
	f.connectivity.link_up("192.0.2.10");
	CHECK(f.connectivity.mode() == NetworkMode::normal);
	CHECK(f.connectivity.failures() == 0);
	f.run_until(1000000);
	CHECK(f.connectivity.state() == LinkState::up);
	CHECK(f.loop.timer_count() == 0); // no phase timer left
}

TEST_CASE("Connectivity.PairingSuccessEndsTheFailsafe") {
	Fixture f;
	f.connectivity.start();
	f.fail_attempts_until(70000);
	REQUIRE(f.connectivity.mode() == NetworkMode::failsafe_pairing);
	f.connectivity.pairing_succeeded("new router");
	CHECK(f.connectivity.mode() == NetworkMode::normal);
	CHECK_FALSE(f.connectivity.pairing_active());
	f.run_until(70500);
	CHECK(f.driver.connects.back() == 70500);
	f.connectivity.link_up("192.0.2.20");
	f.run_until(1000000);
	CHECK(f.loop.timer_count() == 0);
}

TEST_CASE("Connectivity.PairingFailureInTheFailsafeRestartsWithinThePhase") {
	Fixture f;
	f.connectivity.start();
	f.fail_attempts_until(70000);
	f.connectivity.pairing_failed("WPS overlap");
	CHECK(f.connectivity.pairing_active()); // the restart counts as pairing
	f.run_until(71000);
	CHECK(f.driver.pairings == std::vector<uint64_t>{64500, 71000});
	CHECK(f.connectivity.mode() == NetworkMode::failsafe_pairing);
}

// ---- Pairing without credentials -------------------------------------------------------------

TEST_CASE("Connectivity.WithoutCredentialsItPairsAtOnce") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	CHECK(f.driver.connects.empty());
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000});
	CHECK(f.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(f.connectivity.pairing_active());
	CHECK(f.capture.sink.contains("W hc.net: no credentials stored: pairing until a partner answers"));
	CHECK(f.capture.sink.contains("I hc.net: network pairing (unprovisioned): press the pairing (WPS) button"));
}

TEST_CASE("Connectivity.UnprovisionedPairingRestartsAfterEveryFailure") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	f.clock.now_ms = 121000;
	f.connectivity.pairing_failed("timed out");
	f.connectivity.pairing_failed("failed"); // the IDF's duplicate report collapses
	CHECK(f.driver.pairing_stops == 1);
	CHECK(f.capture.sink.contains("W hc.net: pairing failed: timed out; listening again in 1 s"));
	f.run_until(122000);
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000, 122000});
	// Without any report from the driver, the core's own pairing timeout restarts it.
	f.run_until(122000 + Connectivity::kPairingTimeoutMs + Connectivity::kPairingRestartMs);
	CHECK(f.driver.pairings.size() == 3);
	CHECK(f.capture.sink.contains("pairing failed: no pairing outcome after 150 s"));
	CHECK(f.driver.connects.empty());
}

TEST_CASE("Connectivity.UnprovisionedPairingSuccessConnects") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	f.clock.advance(30000);
	f.driver.has_credentials = true; // the driver stored them before reporting
	f.connectivity.pairing_succeeded("credentials for \"Example\"");
	CHECK(f.capture.sink.contains("I hc.net: pairing succeeded: credentials for \"Example\""));
	CHECK(f.driver.calls.back() == "unpair@31000");
	f.run_until(31499);
	CHECK(f.driver.connects.empty());
	f.run_until(31500);
	CHECK(f.driver.connects == std::vector<uint64_t>{31500});
	CHECK(f.connectivity.mode() == NetworkMode::normal);
	CHECK(f.connectivity.state() == LinkState::connecting);
}

TEST_CASE("Connectivity.PairingThatCannotStartIsRetriedWithBackoff") {
	Fixture f;
	f.driver.has_credentials = false;
	f.driver.pairing_starts = false;
	f.connectivity.start();
	f.run_until(1000 + 1000 + 2000);
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000, 2000, 4000});
	CHECK(f.capture.sink.contains("W hc.net: pairing could not start; trying again in 2 s"));
	CHECK_FALSE(f.connectivity.request_pairing("console")); // the retry counts as active
}

TEST_CASE("Connectivity.RefusedConnectFallsBackToPairing") {
	Fixture f;
	f.driver.result = ConnectResult::refused;
	f.connectivity.start();
	CHECK(f.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000});
}

// ---- Pairing on request ------------------------------------------------------------------------

TEST_CASE("Connectivity.RepairingKeepsTheLinkUntilCredentialsArrive") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.clock.advance(1000);
	CHECK(f.connectivity.request_pairing("button held"));
	CHECK(f.driver.calls.back() == "pair@2000");
	CHECK(f.driver.disconnects == 0);
	CHECK(f.connectivity.state() == LinkState::up);
	CHECK(f.connectivity.mode() == NetworkMode::repairing);
	CHECK_FALSE(f.connectivity.request_pairing("console"));
	CHECK(f.capture.sink.contains("W hc.net: refusing pairing (console): pairing is already active"));

	f.clock.advance(5000);
	f.connectivity.pairing_succeeded("new credentials");
	f.run_until(7500);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 7500}); // reconnect with the new credentials
	CHECK(f.connectivity.mode() == NetworkMode::normal);
}

TEST_CASE("Connectivity.FailedRepairingKeepsTheOldLink") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.connectivity.request_pairing("console");
	f.clock.advance(120000);
	f.connectivity.pairing_failed("timed out");
	CHECK(f.capture.sink.contains("W hc.net: pairing failed: timed out; back to the stored credentials"));
	CHECK(f.connectivity.mode() == NetworkMode::normal);
	CHECK(f.connectivity.state() == LinkState::up);
	f.run_until(1000000);
	CHECK(f.driver.connects.size() == 1);
}

TEST_CASE("Connectivity.LinkLostDuringRepairingIsNoFailureAndHealsAfterwards") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.connectivity.request_pairing("console");
	f.clock.advance(10000);
	// The WPS exchange leaves the access point.
	f.connectivity.link_down("left the access point for the WPS exchange");
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(f.connectivity.failures() == 0);
	CHECK(f.capture.sink.contains("W hc.net: network down: left the access point for the WPS exchange; pairing"));
	f.run_until(20000);
	CHECK(f.driver.connects.size() == 1); // no reconnect while pairing
	f.connectivity.pairing_failed("failed");
	f.run_until(20500);
	CHECK(f.driver.connects.back() == 20500);
}

TEST_CASE("Connectivity.RepairingDuringAnAttemptAbortsItFirst") {
	Fixture f;
	f.connectivity.start(); // connecting
	f.clock.advance(100);
	f.connectivity.request_pairing("console");
	CHECK(f.driver.calls.back() == "disconnect@1100");
	f.run_until(1600);
	CHECK(f.driver.calls.back() == "pair@1600");
}

TEST_CASE("Connectivity.RequestConnectEndsPairingAndConnects") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	f.clock.advance(5000);
	f.driver.has_credentials = true; // "wifi set" stored them
	f.connectivity.request_connect("new WiFi credentials");
	CHECK(f.driver.calls.back() == "unpair@6000");
	CHECK_FALSE(f.connectivity.pairing_active());
	f.run_until(6500);
	CHECK(f.driver.connects == std::vector<uint64_t>{6500});
}

TEST_CASE("Connectivity.RequestConnectWithoutCredentialsPairs") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	f.driver.has_credentials = false; // "wifi clear"
	f.clock.advance(1000);
	f.connectivity.request_connect("WiFi credentials cleared");
	CHECK(f.driver.calls.back() == "disconnect@2000");
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(f.connectivity.mode() == NetworkMode::unprovisioned);
	f.run_until(2500);
	CHECK(f.driver.pairings == std::vector<uint64_t>{2500});
}

TEST_CASE("Connectivity.DownReportsWhileWaitingAreNoFailures") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_down("first");
	f.connectivity.link_down("late duplicate");
	CHECK(f.connectivity.failures() == 1);
	f.driver.has_credentials = false;
	f.connectivity.request_connect("cleared");
	f.connectivity.link_down("from the stopped attempt");
	CHECK(f.connectivity.failures() == 0);
}

// ---- Pausing for a scan -------------------------------------------------------------------------

TEST_CASE("Connectivity.PauseStopsPairingAndResumeRestartsIt") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	REQUIRE(f.driver.pairings == std::vector<uint64_t>{1000});
	f.clock.now_ms = 3000;
	CHECK(f.connectivity.pause_pairing("WiFi scan"));
	CHECK(f.driver.pairing_stops == 1);
	CHECK(f.connectivity.paused());
	CHECK(f.connectivity.pairing_active()); // held, about to restart: the LED keeps showing pairing
	CHECK(f.connectivity.station_free_at() == 3000 + Connectivity::kSettleMs);
	CHECK(f.capture.sink.contains("I hc.net: network paused for WiFi scan: pairing stopped, resumes afterwards"));
	f.run_until(3000 + Connectivity::kMaxPauseMs - 1); // no restart while paused
	CHECK(f.driver.pairings.size() == 1);
	CHECK(f.connectivity.paused());
	f.run_until(3000 + Connectivity::kMaxPauseMs + Connectivity::kSettleMs);
	CHECK_FALSE(f.connectivity.paused()); // the pause limit resumed it
	CHECK(f.capture.sink.contains("W hc.net: network pause for WiFi scan not ended after 20 s: resuming"));
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000, 3000 + Connectivity::kMaxPauseMs + Connectivity::kSettleMs});
}

TEST_CASE("Connectivity.ResumeRunsTheHeldPairingAfterTheSettleTime") {
	Fixture f;
	f.driver.has_credentials = false;
	f.connectivity.start();
	f.clock.now_ms = 3000;
	f.connectivity.pause_pairing("WiFi scan");
	f.clock.now_ms = 6000;
	f.connectivity.resume_pairing("scan over");
	CHECK_FALSE(f.connectivity.paused());
	CHECK(f.connectivity.next_action_at() == 6000 + Connectivity::kSettleMs);
	f.run_until(6000 + Connectivity::kSettleMs);
	CHECK(f.driver.pairings == std::vector<uint64_t>{1000, 6000 + Connectivity::kSettleMs});
	CHECK(f.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(f.capture.sink.contains("I hc.net: network resumed (scan over): pairing"));
}

TEST_CASE("Connectivity.PauseAbortsAnAttemptAndResumeRepeatsIt") {
	Fixture f;
	f.connectivity.start();
	REQUIRE(f.connectivity.state() == LinkState::connecting);
	f.clock.now_ms = 2000;
	f.connectivity.pause_pairing("WiFi scan");
	CHECK(f.driver.disconnects == 1);
	CHECK(f.connectivity.state() == LinkState::down);
	CHECK(std::string(f.connectivity.reason()) == "paused for WiFi scan");
	CHECK(f.connectivity.failures() == 0); // not a failure
	CHECK(f.connectivity.station_free_at() == 2000 + Connectivity::kSettleMs);
	f.connectivity.link_down("wifi reason 8 (ASSOC_LEAVE)"); // the abort's own report
	CHECK(f.connectivity.failures() == 0);
	f.clock.now_ms = 5000;
	f.connectivity.resume_pairing("scan over");
	f.run_until(5000 + Connectivity::kSettleMs);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000, 5000 + Connectivity::kSettleMs});
	CHECK(f.connectivity.state() == LinkState::connecting);
}

TEST_CASE("Connectivity.StepsDueWhilePausedAreHeldAndTheLatestWins") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_down("no access point"); // reconnect in 1 s
	f.connectivity.pause_pairing("WiFi scan");
	CHECK(f.connectivity.station_free_at() == 1000); // nothing was running
	f.run_until(4000);
	CHECK(f.driver.connects.size() == 1); // the reconnect came due and was held
	CHECK(f.capture.sink.contains("I hc.net: network: connect held while paused for WiFi scan"));

	// A request meanwhile is decided as usual; its step is held too and replaces the old one.
	f.driver.has_credentials = false;
	f.connectivity.request_connect("credentials cleared");
	CHECK(f.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(f.connectivity.pairing_active());
	CHECK(f.driver.pairings.empty());
	CHECK_FALSE(f.connectivity.request_pairing("console")); // already about to pair
	f.connectivity.resume_pairing("scan over");
	f.run_until(4000 + Connectivity::kSettleMs);
	CHECK(f.driver.pairings == std::vector<uint64_t>{4000 + Connectivity::kSettleMs});
	CHECK(f.driver.connects.size() == 1);
}

TEST_CASE("Connectivity.PauseWithTheLinkUpLeavesItAlone") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	CHECK(f.connectivity.pause_pairing("WiFi scan"));
	CHECK(f.driver.disconnects == 0);
	CHECK(f.connectivity.is_up());
	CHECK(f.connectivity.station_free_at() == 1000);
	CHECK_FALSE(f.connectivity.pause_pairing("another scan"));
	CHECK(f.capture.sink.contains("W hc.net: refusing pause for another scan: already paused for WiFi scan"));
	f.connectivity.resume_pairing("scan over");
	CHECK(f.loop.timer_count() == 0);
	f.connectivity.resume_pairing("scan over");
	CHECK(f.capture.sink.contains("W hc.net: resume (scan over) while not paused, ignored"));

	// The link coming up during a pause drops a held reconnect.
	f.connectivity.link_down("beacon timeout");
	f.connectivity.pause_pairing("WiFi scan");
	f.run_until(3000);
	f.connectivity.link_up("192.0.2.10");
	f.connectivity.resume_pairing("scan over");
	f.run_until(10000);
	CHECK(f.driver.connects.size() == 1);
	CHECK(f.connectivity.is_up());
}

TEST_CASE("Connectivity.RepairingPausedKeepsTheLink") {
	Fixture f;
	f.connectivity.start();
	f.connectivity.link_up("192.0.2.10");
	REQUIRE(f.connectivity.request_pairing("button"));
	REQUIRE(f.driver.pairings.size() == 1);
	f.connectivity.pause_pairing("WiFi scan");
	CHECK(f.driver.pairing_stops == 1);
	CHECK(f.connectivity.is_up());
	f.connectivity.resume_pairing("scan over");
	f.run_until(f.clock.now_ms + Connectivity::kSettleMs);
	CHECK(f.driver.pairings.size() == 2);
	CHECK(f.connectivity.mode() == NetworkMode::repairing);
	CHECK(f.connectivity.is_up());
}
