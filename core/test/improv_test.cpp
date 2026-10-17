/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Improv Wi-Fi serial service under the fake clock: the RPCs against the connection workflow,
// the scan with its pairing pause, the error answers and the lifetime rule.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/controller.h"
#include "hc/event_loop.h"
#include "hc/improv.h"
#include "hc/improv_protocol.h"
#include "hc/network_settings.h"
#include "hc/version.h"
#include "test_support.h"

using hc::Connectivity;
using hc::EventLoop;
using hc::Improv;
using hc::ImprovReader;
using hc::LinkState;
using hc_test::FakeClock;
using hc_test::FakeImprovPort;
using hc_test::FakeNetworkDriver;
using hc_test::FakeScanner;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;
namespace improv = hc::improv;

namespace {

using Bytes = std::vector<uint8_t>;

/// A driver whose credentials are the stored settings, as on the ESP.
class SettingsDriver : public FakeNetworkDriver {
public:
	SettingsDriver(const FakeClock& clock, const FakeSettings& settings_) : FakeNetworkDriver(clock), settings(settings_) {}

	bool provisioned() const override { return settings.entries.count(hc::setting::kWifiSsid) > 0; }

	const FakeSettings& settings;
};

/// A client frame with its checksum.
Bytes client_frame(uint8_t type, const Bytes& data) {
	Bytes f = {'I', 'M', 'P', 'R', 'O', 'V', improv::kVersion, type, static_cast<uint8_t>(data.size())};
	f.insert(f.end(), data.begin(), data.end());
	f.push_back(improv::checksum(f.data(), f.size()));
	return f;
}

Bytes rpc(uint8_t command, const Bytes& payload = {}) {
	Bytes data = {command, static_cast<uint8_t>(payload.size())};
	data.insert(data.end(), payload.begin(), payload.end());
	return client_frame(improv::kTypeRpcCommand, data);
}

Bytes wifi_settings(const std::string& ssid, const std::string& psk) {
	Bytes payload = {static_cast<uint8_t>(ssid.size())};
	payload.insert(payload.end(), ssid.begin(), ssid.end());
	payload.push_back(static_cast<uint8_t>(psk.size()));
	payload.insert(payload.end(), psk.begin(), psk.end());
	return rpc(improv::kRpcWifiSettings, payload);
}

Bytes state_frame(improv::State state) {
	uint8_t out[improv::kMaxFrame];
	size_t n = improv::build_current_state(state, out, sizeof(out));
	return Bytes(out, out + n);
}

Bytes error_frame(improv::Error error) {
	uint8_t out[improv::kMaxFrame];
	size_t n = improv::build_error(error, out, sizeof(out));
	return Bytes(out, out + n);
}

Bytes result_frame(uint8_t command, std::vector<std::string> strings) {
	std::vector<const char*> pointers;
	for (const std::string& s : strings) {
		pointers.push_back(s.c_str());
	}
	uint8_t out[improv::kMaxFrame];
	size_t n = improv::build_rpc_result(command, pointers.data(), pointers.size(), out, sizeof(out));
	return Bytes(out, out + n);
}

struct Fixture {
	ScopedCapture capture;
	FakeClock clock;
	EventLoop loop;
	FakeSettings settings;
	SettingsDriver driver{clock, settings};
	Connectivity connectivity{loop, clock, driver};
	FakeImprovPort port;
	FakeScanner scanner;
	Improv improv{loop, clock, settings, connectivity, &port, &scanner};
	ImprovReader reader;

	Fixture(bool provisioned = false) {
		if (provisioned) {
			settings.set_string(hc::setting::kWifiSsid, "Example Net");
			settings.set_string(hc::setting::kWifiPsk, "example-key");
		}
		improv.start();
		connectivity.start();
	}

	/// Sends bytes through the reader into the service, as the ESP reader task and loop do.
	void receive(const Bytes& bytes) {
		for (uint8_t b : bytes) {
			switch (reader.feed(b, clock.now_ms)) {
			case ImprovReader::Event::frame:
				improv.handle_frame(reader.frame_type(), reader.frame_data(), reader.frame_length());
				break;
			case ImprovReader::Event::bad_frame:
				improv.bad_frame(reader.bad_reason());
				break;
			default:
				break;
			}
		}
	}

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

	/// The frames sent since the last call.
	std::vector<Bytes> sent() {
		std::vector<Bytes> result = port.frames;
		port.frames.clear();
		return result;
	}
};

const std::string kUrl = "http://homecontrol-a1b2c3.local/";

} // namespace

TEST_CASE("Improv.SilentUntilAClientSpeaks") {
	Fixture f;
	f.run_until(60000);
	CHECK(f.port.frames.empty());
	CHECK(f.improv.active());
	CHECK(f.capture.sink.contains("I hc.improv: Improv Wi-Fi serial active for 300 s (ends 60 s after the network is "
		"first up)"));
}

TEST_CASE("Improv.CurrentStateAndDeviceInfo") {
	Fixture f;
	f.receive(rpc(improv::kRpcGetCurrentState));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none), state_frame(improv::State::authorized)});

	f.receive(rpc(improv::kRpcGetDeviceInfo));
	std::string version = std::string(hc::kVersion) + " (" + hc::build_id() + ")";
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none),
		result_frame(improv::kRpcGetDeviceInfo, {"HomeControl", version, "ESP32-C6", "homecontrol-a1b2c3"})});

	// Provisioned: the state is followed by the device URL.
	Fixture p(true);
	p.connectivity.link_up("192.0.2.10");
	p.receive(rpc(improv::kRpcGetCurrentState));
	CHECK(p.sent() == std::vector<Bytes>{error_frame(improv::Error::none), state_frame(improv::State::provisioned),
		result_frame(improv::kRpcGetCurrentState, {kUrl})});
}

TEST_CASE("Improv.WifiSettingsStoreConnectAndAnswerWithTheUrl") {
	Fixture f; // unprovisioned: pairing
	REQUIRE(f.connectivity.pairing_active());
	f.receive(wifi_settings("Example Net", "example-key"));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none), state_frame(improv::State::provisioning)});
	CHECK(f.settings.entries.at("wifi.ssid").text == "Example Net");
	CHECK(f.settings.entries.at("wifi.psk").text == "example-key");
	CHECK(f.improv.provisioning());
	CHECK(f.improv.state() == improv::State::provisioning);
	CHECK(f.capture.sink.contains("I hc.improv: Improv: credentials received for Example Net, connecting"));
	CHECK_FALSE(f.capture.sink.contains("example-key"));

	// Pairing stops; the connect follows after the settle time.
	f.run_until(1000 + Connectivity::kSettleMs);
	CHECK(f.driver.connects == std::vector<uint64_t>{1000 + Connectivity::kSettleMs});
	CHECK(f.connectivity.state() == LinkState::connecting);
	CHECK(f.port.frames.empty());

	f.connectivity.link_up("192.0.2.10");
	CHECK_FALSE(f.improv.provisioning());
	CHECK(f.sent() == std::vector<Bytes>{state_frame(improv::State::provisioned),
		result_frame(improv::kRpcWifiSettings, {kUrl})});
	CHECK(f.capture.sink.contains("I hc.improv: Improv: provisioned (link up), device at " + kUrl));
}

TEST_CASE("Improv.WifiSettingsThatDoNotConnectAreReportedAfterThreeFailures") {
	Fixture f(true);
	f.connectivity.link_up("192.0.2.10");
	f.receive(wifi_settings("Other Net", "wrong-key"));
	f.sent();
	// request_connect drops the link and reconnects at once.
	REQUIRE(f.connectivity.state() == LinkState::connecting);
	for (uint32_t i = 0; i < Improv::kProvisionFailures; ++i) {
		REQUIRE(f.improv.provisioning());
		f.connectivity.link_down("wifi reason 15 (4WAY_HANDSHAKE_TIMEOUT (wrong key?))");
		f.run_until(f.connectivity.next_action_at());
	}
	CHECK_FALSE(f.improv.provisioning());
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::unable_to_connect),
		state_frame(improv::State::authorized)});
	CHECK(f.capture.sink.contains("Improv: provisioning failed: 3 attempts failed"));
	CHECK(f.settings.entries.at("wifi.ssid").text == "Other Net"); // stays stored
}

TEST_CASE("Improv.WifiSettingsWithoutOutcomeTimeOut") {
	Fixture f;
	f.receive(wifi_settings("Example Net", "example-key"));
	f.sent();
	f.run_until(1000 + Improv::kProvisionTimeoutMs - 1);
	CHECK(f.improv.provisioning());
	f.run_until(1000 + Improv::kProvisionTimeoutMs);
	CHECK_FALSE(f.improv.provisioning());
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::unable_to_connect),
		state_frame(improv::State::authorized)});
	CHECK(f.capture.sink.contains("Improv: provisioning failed: no link after 25 s; the credentials stay stored"));
}

TEST_CASE("Improv.RefusedCredentialsAreAnsweredAndNotStored") {
	Fixture f;
	f.receive(wifi_settings("Example Net", "short"));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none),
		error_frame(improv::Error::unable_to_connect)});
	CHECK(f.settings.entries.empty());
	CHECK_FALSE(f.improv.provisioning());
	CHECK(f.capture.sink.contains("Improv: credentials for Example Net refused"));

	// Lengths that do not add up: invalid RPC.
	f.receive(rpc(improv::kRpcWifiSettings, {3, 'N', 'e', 't', 9, 'x'}));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none), error_frame(improv::Error::invalid_rpc)});
}

TEST_CASE("Improv.MalformedFramesAreAnsweredAndTheParserResynchronizes") {
	Fixture f;
	Bytes broken = rpc(improv::kRpcGetCurrentState);
	broken.back() ^= 0x01;
	f.receive(broken);
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::invalid_rpc)});
	CHECK(f.capture.sink.contains("W hc.improv: Improv frame refused: checksum mismatch"));

	// Noise, then a good frame.
	Bytes next = {'x', 'I', 'M'};
	Bytes good = rpc(improv::kRpcGetCurrentState);
	next.insert(next.end(), good.begin(), good.end());
	f.receive(next);
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none), state_frame(improv::State::authorized)});

	// An unknown command, an RPC whose length byte lies, a frame of a device-side type.
	f.receive(rpc(0x09));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none), error_frame(improv::Error::unknown_rpc)});
	f.receive(client_frame(improv::kTypeRpcCommand, {improv::kRpcGetCurrentState, 5}));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::invalid_rpc)});
	f.receive(client_frame(improv::kTypeCurrentState, {0x02}));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::invalid_rpc)});
}

TEST_CASE("Improv.ScanPausesPairingAndStreamsTheNetworks") {
	Fixture f; // unprovisioned: pairing since 1000
	REQUIRE(f.driver.pairings == std::vector<uint64_t>{1000});
	f.clock.now_ms = 2000;
	f.receive(rpc(improv::kRpcGetWifiNetworks));
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none)});
	CHECK(f.driver.pairing_stops == 1); // WPS owns the station: stopped for the scan
	CHECK(f.connectivity.paused());
	CHECK(f.connectivity.pairing_active()); // the LED keeps showing pairing
	CHECK(f.scanner.starts == 0);

	f.run_until(2000 + Connectivity::kSettleMs);
	REQUIRE(f.scanner.starts == 1);
	f.scanner.sink->scan_network("Alpha", -70, true);
	f.scanner.sink->scan_network("Beta", -50, false);
	f.scanner.sink->scan_network("Alpha", -60, true); // second access point of a mesh
	f.scanner.sink->scan_network("", -40, true);      // hidden
	f.scanner.sink->scan_finished(true, "");
	CHECK(f.sent() == std::vector<Bytes>{result_frame(improv::kRpcGetWifiNetworks, {"Beta", "-50", "NO"}),
		result_frame(improv::kRpcGetWifiNetworks, {"Alpha", "-60", "YES"}),
		result_frame(improv::kRpcGetWifiNetworks, {})});
	CHECK(f.capture.sink.contains("Improv: WiFi scan reported 2 network(s)"));
	CHECK_FALSE(f.connectivity.paused());

	// Pairing resumes after the settle time.
	f.run_until(f.clock.now_ms + Connectivity::kSettleMs);
	CHECK(f.driver.pairings.size() == 2);
	CHECK(f.connectivity.pairing_active());
}

TEST_CASE("Improv.ScanKeepsTheStrongestNetworks") {
	Fixture f(true);
	f.connectivity.link_up("192.0.2.10");
	f.receive(rpc(improv::kRpcGetWifiNetworks));
	f.run_until(f.clock.now_ms); // the station is free at once: link up, nothing to stop
	REQUIRE(f.scanner.starts == 1);
	f.sent();
	for (int i = 0; i < 25; ++i) {
		std::string name = "net" + std::to_string(i);
		f.scanner.sink->scan_network(name.c_str(), static_cast<int8_t>(-90 + i), true);
	}
	f.scanner.sink->scan_finished(true, "");
	std::vector<Bytes> frames = f.sent();
	REQUIRE(frames.size() == Improv::kMaxNetworks + 1);
	CHECK(frames.front() == result_frame(improv::kRpcGetWifiNetworks, {"net24", "-66", "YES"}));
	CHECK(frames[Improv::kMaxNetworks - 1] == result_frame(improv::kRpcGetWifiNetworks, {"net5", "-85", "YES"}));
	CHECK(f.driver.connects.size() == 1); // the link was left alone
}

TEST_CASE("Improv.FailedScansAnswerAnEmptyListAndResume") {
	Fixture f;
	f.scanner.accept = false;
	f.receive(rpc(improv::kRpcGetWifiNetworks));
	f.run_until(1000 + Connectivity::kSettleMs);
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none),
		result_frame(improv::kRpcGetWifiNetworks, {})});
	CHECK(f.capture.sink.contains("Improv: WiFi scan failed: the platform could not start it"));
	CHECK_FALSE(f.connectivity.paused());

	// A scan that never reports.
	f.scanner.accept = true;
	uint64_t at = f.clock.now_ms;
	f.receive(rpc(improv::kRpcGetWifiNetworks));
	f.run_until(at + Improv::kScanTimeoutMs);
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::none),
		result_frame(improv::kRpcGetWifiNetworks, {})});
	CHECK(f.capture.sink.contains("Improv: WiFi scan failed: no result after 15 s"));
	CHECK_FALSE(f.connectivity.paused());
	// A late report is ignored.
	f.scanner.sink->scan_network("Late", -40, true);
	f.scanner.sink->scan_finished(true, "");
	CHECK(f.port.frames.empty());
}

TEST_CASE("Improv.EndsWhenTheWindowIsOver") {
	Fixture f;
	CHECK(f.improv.ends_at() == 1000 + Improv::kWindowMs);
	f.run_until(1000 + Improv::kWindowMs - 1);
	CHECK(f.improv.active());
	f.run_until(1000 + Improv::kWindowMs);
	CHECK_FALSE(f.improv.active());
	CHECK(f.port.ended == 1);
	CHECK(f.capture.sink.contains("I hc.improv: Improv Wi-Fi serial ended (the window after boot is over): the serial "
		"port belongs to the console"));
	f.receive(rpc(improv::kRpcGetCurrentState));
	CHECK(f.port.frames.empty());
	CHECK(f.capture.sink.contains("Improv frame ignored: Improv is over"));
}

TEST_CASE("Improv.EndsAGracePeriodAfterTheLinkFirstComesUp") {
	Fixture f(true);
	f.clock.now_ms = 10000;
	f.connectivity.link_up("192.0.2.10");
	CHECK(f.improv.ends_at() == 10000 + Improv::kGraceMs);
	CHECK(f.capture.sink.contains("network up: Improv ends in 60 s"));
	// A later loss and return does not move it.
	f.clock.now_ms = 30000;
	f.connectivity.link_down("beacon timeout");
	f.connectivity.link_up("192.0.2.10");
	CHECK(f.improv.ends_at() == 10000 + Improv::kGraceMs);
	f.run_until(10000 + Improv::kGraceMs);
	CHECK_FALSE(f.improv.active());
	CHECK(f.capture.sink.contains("Improv Wi-Fi serial ended (the network is up)"));
}

TEST_CASE("Improv.ClientFramesKeepItAliveForTheGracePeriod") {
	Fixture f(true);
	f.clock.now_ms = 10000;
	f.connectivity.link_up("192.0.2.10");
	f.clock.now_ms = 60000;
	f.receive(rpc(improv::kRpcGetDeviceInfo));
	CHECK(f.improv.ends_at() == 60000 + Improv::kGraceMs);
	f.run_until(60000 + Improv::kGraceMs - 1);
	CHECK(f.improv.active());
	f.run_until(60000 + Improv::kGraceMs);
	CHECK_FALSE(f.improv.active());

	// Activity near the end of the window extends it too.
	Fixture w;
	w.clock.now_ms = Improv::kWindowMs - 1000;
	w.receive(rpc(improv::kRpcGetCurrentState));
	CHECK(w.improv.ends_at() == Improv::kWindowMs - 1000 + Improv::kGraceMs);
}

TEST_CASE("Improv.AWifiSettingsExchangeCompletesBeforeTheEnd") {
	Fixture f;
	uint64_t window_end = 1000 + Improv::kWindowMs;
	f.clock.now_ms = window_end - 1000;
	f.receive(wifi_settings("Example Net", "example-key"));
	CHECK(f.improv.ends_at() == window_end - 1000 + Improv::kGraceMs);
	f.sent();
	// No link: the exchange fails at its timeout, while Improv still runs, and the client gets
	// another grace period to read the outcome.
	uint64_t timeout = window_end - 1000 + Improv::kProvisionTimeoutMs;
	f.run_until(timeout);
	CHECK(f.improv.active());
	CHECK(f.sent() == std::vector<Bytes>{error_frame(improv::Error::unable_to_connect),
		state_frame(improv::State::authorized)});
	CHECK(f.improv.ends_at() == timeout + Improv::kGraceMs);
	f.run_until(timeout + Improv::kGraceMs);
	CHECK_FALSE(f.improv.active());
	CHECK(f.port.ended == 1);
}

TEST_CASE("Improv.AScanCompletesBeforeTheEnd") {
	Fixture f;
	uint64_t at = 1000 + Improv::kWindowMs - 1000;
	f.clock.now_ms = at;
	f.receive(rpc(improv::kRpcGetWifiNetworks)); // keeps Improv until at + kGraceMs
	REQUIRE(f.connectivity.paused());
	f.run_until(at + Connectivity::kSettleMs);
	REQUIRE(f.scanner.starts == 1);
	f.sent();
	// The platform never reports: the scan timeout answers while Improv still runs.
	f.run_until(at + Improv::kScanTimeoutMs);
	CHECK(f.improv.active());
	CHECK(f.sent() == std::vector<Bytes>{result_frame(improv::kRpcGetWifiNetworks, {})});
	CHECK_FALSE(f.connectivity.paused());
	f.run_until(at + Improv::kGraceMs);
	CHECK_FALSE(f.improv.active());
}

TEST_CASE("Improv.WithoutAPortTheServiceIsOff") {
	ScopedCapture capture;
	FakeClock clock;
	EventLoop loop;
	FakeSettings settings;
	FakeNetworkDriver driver{clock};
	Connectivity connectivity{loop, clock, driver};
	Improv improv{loop, clock, settings, connectivity, nullptr, nullptr};
	improv.start();
	CHECK_FALSE(improv.active());
	CHECK(capture.sink.contains("Improv Wi-Fi serial not offered: no serial port for it on this platform"));
	uint8_t data[] = {improv::kRpcGetCurrentState, 0};
	improv.handle_frame(improv::kTypeRpcCommand, data, sizeof(data));
	CHECK(capture.sink.contains("Improv frame ignored: Improv is over"));
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("Improv.ControllerShowsPairingWhileProvisioning") {
	hc_test::FakePlatform fake;
	FakeImprovPort port;
	FakeScanner scanner;
	hc::Platform platform = fake.platform();
	platform.improv = &port;
	platform.scanner = &scanner;
	hc::Controller controller(platform);
	controller.start(); // credentials stored (the fake driver reports provisioned): connecting
	CHECK(controller.led_status() == hc::LedStatus::connecting);
	uint8_t payload[] = {improv::kRpcWifiSettings, 13, 3, 'N', 'e', 't', 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2'};
	controller.improv().handle_frame(improv::kTypeRpcCommand, payload, sizeof(payload));
	CHECK(controller.improv().provisioning());
	CHECK(controller.led_status() == hc::LedStatus::pairing);
	controller.connectivity().link_up("192.0.2.10");
	CHECK(controller.led_status() == hc::LedStatus::setup);
}
