/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the network bring-up (issue #2): they compose the
// connection workflow, the hold button, the controller's LED derivation and the credential store
// in ways the package was not written against. See the workflow skill, section 4.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/controller.h"
#include "hc/event_loop.h"
#include "hc/hold_button.h"
#include "hc/network_settings.h"
#include "test_support.h"

using hc::Connectivity;
using hc::EventLoop;
using hc::LedStatus;
using hc::LinkState;
using hc::NetworkMode;
using hc_test::FakeClock;
using hc_test::FakeNetworkDriver;
using hc_test::ScopedCapture;

namespace {

struct Net {
	ScopedCapture capture;
	FakeClock clock;
	EventLoop loop;
	FakeNetworkDriver driver{clock};
	Connectivity connectivity{loop, clock, driver};

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

	/// Fails every attempt as soon as it starts, until `until_ms`.
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

} // namespace

TEST_CASE("NetworkProbe.LifeStoryFromUnprovisionedThroughFailsafeToRepairing") {
	Net n;
	n.driver.has_credentials = false;
	n.connectivity.start();
	// 1. Fresh board: pairing at once, and again after a failed round.
	REQUIRE(n.driver.pairings.size() == 1);
	CHECK(n.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(n.connectivity.pairing_active());
	n.connectivity.pairing_failed("WPS timeout");
	CHECK(n.connectivity.pairing_active()); // about to restart: the LED keeps blinking fast
	n.run_until(n.clock.now_ms + Connectivity::kPairingRestartMs + Connectivity::kSettleMs + 1);
	CHECK(n.driver.pairings.size() == 2);
	CHECK(n.driver.connects.empty());

	// 2. The router answers: credentials stored, connect, up.
	n.driver.has_credentials = true;
	n.connectivity.pairing_succeeded("ssid stored");
	n.run_until(n.clock.now_ms + Connectivity::kSettleMs + 1);
	REQUIRE(n.driver.connects.size() == 1);
	CHECK(n.connectivity.state() == LinkState::connecting);
	n.connectivity.link_up("192.0.2.20");
	CHECK(n.connectivity.mode() == NetworkMode::normal);
	CHECK(n.connectivity.failures() == 0);
	CHECK_FALSE(n.connectivity.pairing_active());
	size_t pairings_before = n.driver.pairings.size();

	// 3. The access point disappears for good: backoff, then the failsafe starts pairing.
	n.connectivity.link_down("beacon timeout");
	n.fail_attempts_until(n.clock.now_ms + 200000);
	CHECK(n.connectivity.failures() >= Connectivity::kFailsafeFailures);
	CHECK((n.connectivity.mode() == NetworkMode::failsafe_pairing
		|| n.connectivity.mode() == NetworkMode::failsafe_connecting));
	CHECK(n.driver.pairings.size() > pairings_before);
	CHECK(n.capture.sink.count("failsafe") >= 1);

	// 4. During a failsafe pairing phase the (new) router's button is pressed.
	if (n.connectivity.mode() != NetworkMode::failsafe_pairing) {
		n.run_until(n.clock.now_ms + Connectivity::kFailsafePhaseMs + Connectivity::kSettleMs + 1);
	}
	REQUIRE(n.connectivity.mode() == NetworkMode::failsafe_pairing);
	n.connectivity.pairing_succeeded("new router");
	n.run_until(n.clock.now_ms + Connectivity::kSettleMs + 1);
	CHECK(n.connectivity.state() == LinkState::connecting);
	n.connectivity.link_up("192.0.2.30");
	CHECK(n.connectivity.mode() == NetworkMode::normal);
	CHECK(n.connectivity.failures() == 0);
	CHECK(n.loop.timer_count() == 0); // no phase timer, no timeout, no retry left behind

	// 5. Re-pairing on request keeps the link; a second request is refused; failure keeps the link.
	size_t connects_before = n.driver.connects.size();
	int disconnects_before = n.driver.disconnects;
	CHECK(n.connectivity.request_pairing("button"));
	CHECK(n.connectivity.is_up());
	CHECK(n.connectivity.pairing_active());
	CHECK(n.connectivity.mode() == NetworkMode::repairing);
	n.run_until(n.clock.now_ms + Connectivity::kSettleMs + 1);
	CHECK_FALSE(n.connectivity.request_pairing("console"));
	CHECK(n.capture.sink.count("hc.net") > 0);
	n.connectivity.pairing_failed("WPS timeout");
	n.run_until(n.clock.now_ms + 5000);
	CHECK(n.connectivity.is_up());
	CHECK(n.connectivity.mode() == NetworkMode::normal);
	CHECK(n.driver.connects.size() == connects_before);
	CHECK(n.driver.disconnects == disconnects_before);
	CHECK(n.connectivity.next_action_at() == EventLoop::kNever);

	// 6. `wifi clear`: the link is dropped and the board pairs again.
	n.driver.has_credentials = false;
	n.connectivity.request_connect("credentials cleared");
	n.run_until(n.clock.now_ms + Connectivity::kSettleMs + 1);
	CHECK(n.connectivity.mode() == NetworkMode::unprovisioned);
	CHECK(n.connectivity.pairing_active());
	CHECK(n.driver.disconnects > disconnects_before);
}

TEST_CASE("NetworkProbe.UnprovisionedPairingWithoutAnyReportTimesOutAndRestarts") {
	Net n;
	n.driver.has_credentials = false;
	n.connectivity.start();
	REQUIRE(n.driver.pairings.size() == 1);
	n.run_until(n.clock.now_ms + Connectivity::kPairingTimeoutMs - 1);
	CHECK(n.driver.pairings.size() == 1);
	n.run_until(n.clock.now_ms + Connectivity::kPairingRestartMs + Connectivity::kSettleMs + 2);
	CHECK(n.driver.pairings.size() == 2);
	CHECK(n.driver.pairing_stops >= 1);
	CHECK(n.capture.sink.count("W hc.net") >= 1);
	CHECK(n.connectivity.pairing_active());
}

TEST_CASE("NetworkProbe.ButtonHoldWhileOnlineShowsPairingAndFallsBackToSetup") {
	hc_test::FakePlatform p;
	p.clock.synchronized = true;
	hc_test::FakeStatusLight light(p.clock);
	hc::Platform platform = p.platform();
	platform.status_light = &light;
	hc::Controller controller(platform);
	controller.start();
	controller.connectivity().link_up("192.0.2.40");
	CHECK(controller.led_status() == LedStatus::setup);

	// The platform polls the button every 20 ms; held from 5 s to 8.1 s.
	hc::HoldButton button;
	bool fired = false;
	for (uint64_t t = 0; t < 10000; t += 20) {
		bool pressed = t >= 5000 && t < 8100;
		if (button.sample(pressed, p.clock.now_ms)) {
			CHECK_FALSE(fired); // exactly once
			fired = true;
			CHECK(controller.connectivity().request_pairing("button held"));
		}
		p.clock.advance(20);
		controller.run_once();
	}
	CHECK(fired);
	CHECK(controller.led_status() == LedStatus::pairing);
	CHECK(controller.connectivity().is_up());

	controller.connectivity().pairing_failed("WPS timeout");
	p.clock.advance(Connectivity::kSettleMs + 1);
	controller.run_once();
	CHECK(controller.led_status() == LedStatus::setup);
	CHECK(controller.connectivity().is_up());
	CHECK(p.sink.count("E hc.ctl") == 0);
}

TEST_CASE("NetworkProbe.CredentialStoreEnforcesTheWifiLimits") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	std::string ssid32(32, 'a');
	std::string ssid33(33, 'a');
	std::string psk63(63, 'p');
	std::string hex64(64, 'f');
	std::string nonhex64(64, 'g');

	CHECK(hc::store_wifi_credentials(settings, ssid32.c_str(), psk63.c_str(), &error));
	CHECK(settings.entries.count(hc::setting::kWifiSsid) == 1);
	CHECK(settings.entries.count(hc::setting::kWifiPsk) == 1);

	error = nullptr;
	CHECK_FALSE(hc::store_wifi_credentials(settings, ssid33.c_str(), psk63.c_str(), &error));
	CHECK(error != nullptr);
	CHECK(settings.entries[hc::setting::kWifiSsid].text == ssid32); // nothing changed

	CHECK(hc::store_wifi_credentials(settings, "home", hex64.c_str(), &error));
	error = nullptr;
	CHECK_FALSE(hc::store_wifi_credentials(settings, "home", nonhex64.c_str(), &error));
	CHECK(error != nullptr);
	error = nullptr;
	CHECK_FALSE(hc::store_wifi_credentials(settings, "home", "short", &error));
	CHECK(error != nullptr);
	error = nullptr;
	CHECK_FALSE(hc::store_wifi_credentials(settings, "", psk63.c_str(), &error));
	CHECK(error != nullptr);

	// An open network stores no key at all.
	CHECK(hc::store_wifi_credentials(settings, "cafe", "", &error));
	CHECK(settings.entries.count(hc::setting::kWifiPsk) == 0);

	// A failing store reports, instead of pretending.
	settings.fail_writes = true;
	error = nullptr;
	CHECK_FALSE(hc::store_wifi_credentials(settings, "home", psk63.c_str(), &error));
	CHECK(error != nullptr);

	CHECK(hc::setting_is_secret(hc::setting::kWifiPsk));
	CHECK_FALSE(hc::setting_is_secret(hc::setting::kWifiSsid));
}
