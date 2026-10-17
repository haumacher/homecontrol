/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for Improv Wi-Fi serial (issue #23): console text and frames
// interleaved on one port, provisioning over a running WPS pairing, a scan during provisioning,
// the lifetime edge, and a scan that never answers. See the workflow skill, section 4.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/improv.h"
#include "hc/improv_protocol.h"
#include "hc/network_settings.h"
#include "test_support.h"

using hc::Connectivity;
using hc::EventLoop;
using hc::Improv;
using hc::ImprovReader;
using hc::LinkState;
using hc::NetworkMode;
using hc_test::FakeClock;
using hc_test::FakeImprovPort;
using hc_test::FakeNetworkDriver;
using hc_test::FakeScanner;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;
namespace improv = hc::improv;

namespace {

using Bytes = std::vector<uint8_t>;

class SettingsDriver : public FakeNetworkDriver {
public:
	SettingsDriver(const FakeClock& clock, const FakeSettings& settings_) : FakeNetworkDriver(clock), settings(settings_) {}
	bool provisioned() const override { return settings.entries.count(hc::setting::kWifiSsid) > 0; }
	const FakeSettings& settings;
};

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

struct Probe {
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
	std::string console;

	Probe() {
		improv.start();
		connectivity.start();
	}

	void dispatch(ImprovReader::Event event) {
		switch (event) {
		case ImprovReader::Event::frame:
			improv.handle_frame(reader.frame_type(), reader.frame_data(), reader.frame_length());
			break;
		case ImprovReader::Event::bad_frame:
			improv.bad_frame(reader.bad_reason());
			break;
		case ImprovReader::Event::console:
			console.append(reinterpret_cast<const char*>(reader.console_data()), reader.console_length());
			break;
		default:
			break;
		}
	}

	void receive(const Bytes& bytes) {
		for (uint8_t b : bytes) {
			dispatch(reader.feed(b, clock.now_ms));
		}
	}

	void receive_text(const std::string& text) { receive(Bytes(text.begin(), text.end())); }

	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			dispatch(reader.poll(clock.now_ms));
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				dispatch(reader.poll(clock.now_ms));
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	size_t frames_of_type(uint8_t type) const {
		size_t n = 0;
		for (const Bytes& f : port.frames) {
			if (f.size() > 7 && f[7] == type) {
				++n;
			}
		}
		return n;
	}
};

} // namespace

TEST_CASE("ImprovProbe.ConsoleTextAndASplitFrameShareThePort") {
	Probe p;
	p.receive_text("help\r\n");
	Bytes frame = rpc(improv::kRpcGetCurrentState);
	// The frame arrives in two chunks 30 ms apart, well inside the reader's gap.
	Bytes first(frame.begin(), frame.begin() + 4), second(frame.begin() + 4, frame.end());
	p.receive(first);
	p.clock.advance(30);
	p.receive(second);
	// Then console text that starts like the magic.
	p.receive_text("IMPORT settings\r\n");
	p.run_until(p.clock.now_ms + ImprovReader::kGapMs * 3);
	CHECK(p.console == "help\r\nIMPORT settings\r\n");
	CHECK(p.frames_of_type(improv::kTypeCurrentState) == 1);
	CHECK(p.frames_of_type(improv::kTypeErrorState) == 1); // the "no error" that precedes a reply
	CHECK(p.improv.active());
}

TEST_CASE("ImprovProbe.WifiSettingsOverARunningWpsPairingProvisionsAndEndsPairing") {
	Probe p;
	REQUIRE(p.connectivity.mode() == NetworkMode::unprovisioned);
	REQUIRE(p.connectivity.pairing_active());
	p.receive(wifi_settings("Example Net", "example-key"));
	CHECK(p.improv.provisioning());
	CHECK(p.improv.state() == improv::State::provisioning);
	p.run_until(p.clock.now_ms + Connectivity::kSettleMs + 1);
	REQUIRE(p.driver.connects.size() >= 1);
	CHECK(p.driver.pairing_stops >= 1);
	CHECK(p.connectivity.mode() == NetworkMode::normal);
	p.connectivity.link_up("192.0.2.50");
	p.run_until(p.clock.now_ms + 1);
	CHECK(p.improv.state() == improv::State::provisioned);
	CHECK_FALSE(p.improv.provisioning());
	CHECK_FALSE(p.connectivity.pairing_active());
	CHECK(p.frames_of_type(improv::kTypeRpcResult) == 1); // the URL
	CHECK(p.settings.entries[hc::setting::kWifiSsid].text == "Example Net");

	// Corrected credentials while up: a reconnect, and a second success reply.
	size_t results = p.frames_of_type(improv::kTypeRpcResult);
	p.receive(wifi_settings("Other Net", "other-key-1"));
	p.run_until(p.clock.now_ms + Connectivity::kSettleMs + 1);
	CHECK(p.driver.connects.size() >= 2);
	p.connectivity.link_up("192.0.2.51");
	p.run_until(p.clock.now_ms + 1);
	CHECK(p.frames_of_type(improv::kTypeRpcResult) == results + 1);
	CHECK(p.settings.entries[hc::setting::kWifiSsid].text == "Other Net");
	CHECK(p.capture.sink.count("E hc.improv") == 0);
}

TEST_CASE("ImprovProbe.AScanRequestedDuringProvisioningLeavesNothingStuck") {
	Probe p;
	p.receive(wifi_settings("Example Net", "example-key"));
	p.run_until(p.clock.now_ms + Connectivity::kSettleMs + 1);
	REQUIRE(p.connectivity.state() == LinkState::connecting);
	p.receive(rpc(improv::kRpcGetWifiNetworks));
	p.run_until(p.clock.now_ms + Connectivity::kSettleMs + 1);
	if (p.scanner.sink != nullptr) {
		p.scanner.sink->scan_network("Example Net", -50, true);
		p.scanner.sink->scan_finished(true, "");
	}
	// Whatever the order of answers: half a minute later nothing is paused or scanning, the scan
	// list was terminated, and the connection workflow is alive.
	p.run_until(p.clock.now_ms + 30000);
	CHECK_FALSE(p.connectivity.paused());
	CHECK(p.frames_of_type(improv::kTypeRpcResult) >= 1); // at least the empty list terminator
	bool alive = p.connectivity.next_action_at() != EventLoop::kNever
		|| p.connectivity.state() == LinkState::connecting || p.connectivity.state() == LinkState::up;
	CHECK(alive);
	CHECK(p.loop.timer_count() > 0); // still working, not dead
}

TEST_CASE("ImprovProbe.AFrameJustBeforeTheWindowEndsExtendsItOnce") {
	Probe p;
	uint64_t start = p.clock.now_ms;
	p.run_until(start + Improv::kWindowMs - 1000);
	REQUIRE(p.improv.active());
	p.receive(rpc(improv::kRpcGetCurrentState));
	p.run_until(start + Improv::kWindowMs + 1000);
	CHECK(p.improv.active()); // kept alive by the client's frame
	p.run_until(start + Improv::kWindowMs - 1000 + Improv::kGraceMs + 1);
	CHECK_FALSE(p.improv.active());
	CHECK(p.port.ended == 1);
	size_t frames = p.port.frames.size();
	p.receive(rpc(improv::kRpcGetCurrentState));
	p.run_until(p.clock.now_ms + 1000);
	CHECK(p.port.frames.size() == frames); // ignored after the end
	CHECK(p.port.ended == 1);
	CHECK(p.connectivity.pairing_active()); // the network workflow goes on without Improv
}

TEST_CASE("ImprovProbe.AScanThatNeverAnswersTimesOutAndPairingResumes") {
	Probe p;
	size_t pairings = p.driver.pairings.size();
	p.receive(rpc(improv::kRpcGetWifiNetworks));
	p.run_until(p.clock.now_ms + Connectivity::kSettleMs + 1);
	CHECK(p.connectivity.paused());
	CHECK(p.scanner.starts == 1);
	p.run_until(p.clock.now_ms + Improv::kScanTimeoutMs + Connectivity::kSettleMs + 2000);
	CHECK_FALSE(p.connectivity.paused());
	CHECK(p.frames_of_type(improv::kTypeRpcResult) == 1); // the empty list
	CHECK(p.driver.pairings.size() > pairings);           // WPS listens again
	CHECK(p.capture.sink.count("W hc.improv") >= 1);
}
