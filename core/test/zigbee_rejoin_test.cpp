/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A known device returns by any rejoin while the network is closed (homecontrol #99): the trust
// center's rejoin policy set at the network's start, the link key held for every known device (and
// held again where the stack drops it), and the forced loss of a device (ZDO Mgmt_Leave with rejoin)
// through the adapter and the web API.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "hc/zigbee/key_table.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::zigbee::ZigbeeAdapter;
using hc_test::kBulb;
using hc_test::kBulbShort;
using hc_test::ZigbeeFixture;

namespace {

/// The Philips Hue motion sensor of homecontrol #99 (a legacy HA 1.2 battery end device) and its
/// parent, the Signify plug.
constexpr hc::zigbee::Ieee kSml002 = 0x0017880106498a49ull;
constexpr uint16_t kSml002Short = 0xaf9e;
constexpr hc::zigbee::Ieee kPlug = 0x001788010f069a57ull;
constexpr uint16_t kPlugShort = 0x6b21;

void join_sml002(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80); // an end device
	uint8_t endpoints[] = {2};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSml002Short), 0, endpoints, 1);
	f.descriptor(kSml002Short, 2, hc::zigbee::kHomeAutomationProfile, 0x0107,
		{0x0000, 0x0001, 0x0003, 0x0406, 0x0400, 0x0402}, {0x0019});
	f.basic(kSml002Short, 2, "Philips", "SML002");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kSml002, "Bewegungsmelder Hof"));
}

void join_plug(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kPlug, kPlugShort), 0x8e); // a mains-powered router
	uint8_t endpoints[] = {11};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kPlugShort), 0, endpoints, 1);
	f.descriptor(kPlugShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010A, {0x0000, 0x0003, 0x0006}, {0x0019});
	f.basic(kPlugShort, 11, "Signify Netherlands B.V.", "LOM007");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kPlug, "Schaltsteckdose Zigbee"));
}

/// The device as the adapter view shows it.
std::string view_of(const ZigbeeAdapter& adapter, hc::zigbee::Ieee ieee) {
	for (size_t i = 0; i < adapter.devices().size(); ++i) {
		if (adapter.devices().at(i).ieee == ieee) {
			char buffer[4096];
			hc::JsonWriter json(buffer, sizeof(buffer));
			adapter.write_device_json(json, i);
			REQUIRE_FALSE(json.overflowed());
			return std::string(json.data(), json.length());
		}
	}
	return "";
}

bool contains(const std::string& text, const std::string& part) {
	return text.find(part) != std::string::npos;
}

/// A temperature report of the sensor: a frame that proves it is awake.
void report_temperature(ZigbeeFixture& f) {
	f.report(kSml002Short, 2, hc::zigbee::cluster::kTemperatureMeasurement, 0x0000, hc::zigbee::zcl_type::kInt16, 2150);
}

/// The paired network of the house controller, restarted: the plug and the sensor in the database.
struct HouseFixture : ZigbeeFixture {
	HouseFixture() {
		start_network();
		join_plug(*this);
		join_sml002(*this);
		driver.calls.clear();
	}
};

} // namespace

// ---- The rejoin policy and the link keys at the network's start --------------------------------

TEST_CASE("ZigbeeRejoin: the network's start sets the rejoin policy and holds a key for every known device") {
	HouseFixture f;
	f.driver.keys = {kPlug}; // the plug exchanged its key (Zigbee 3.0), the sensor's was dropped
	hc_test::ScopedCapture capture;
	f.adapter.network_state(false, 0, 0, 0);
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	// The policy first: trust-center rejoins answered, but never for a device without a key.
	REQUIRE(f.driver.calls.size() >= 3);
	CHECK(f.driver.calls[0] == "rejoin-policy tc on, without key off");
	CHECK(f.driver.has("trust 001788010f069a57"));
	CHECK(f.driver.has("trust 0017880106498a49"));
	CHECK(capture.sink.contains("I hc.zigbee: trust center: trust-center rejoins accepted from the devices it holds a "
								"link key for, refused from any other"));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the trust center holds its link key "
								"again at the network's start (the well-known key it joined with): its trust-center "
								"rejoin is accepted"));
	CHECK_FALSE(capture.sink.contains("Schaltsteckdose Zigbee): the trust center holds"));
	CHECK(capture.sink.contains("I hc.zigbee: trust center: holds a link key for 2 of 2 known device(s) (1 restored)"));

	// The next start finds both keys: nothing restored.
	f.driver.calls.clear();
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK(f.driver.count("trust ") == 2);
	CHECK(capture.sink.contains("trust center: holds a link key for 2 of 2 known device(s) (0 restored)"));
}

TEST_CASE("ZigbeeRejoin: a device that left is not trusted; a key the stack does not take is a warning") {
	HouseFixture f;
	f.adapter.device_left(ZigbeeFixture::node(kSml002, kSml002Short)); // left without rejoin (a reset)
	hc_test::ScopedCapture capture;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK(f.driver.has("trust 001788010f069a57"));
	CHECK_FALSE(f.driver.has("trust 0017880106498a49"));
	CHECK(capture.sink.contains("holds a link key for 1 of 1 known device(s) (1 restored)"));

	// The stack refuses (not running): the policy and the keys are warnings, nothing is claimed.
	f.driver.accept = false;
	f.driver.keys.clear();
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK(capture.sink.contains("W hc.zigbee: trust center: the rejoin policy was not set (the stack does not run): its "
								"defaults apply"));
	CHECK(capture.sink.contains("W hc.zigbee: 001788010f069a57 (Schaltsteckdose Zigbee): the trust center holds no link key "
								"for it and did not take one at the network's start (stack answer 0): its trust-center "
								"rejoin is refused"));
	CHECK(capture.sink.contains("holds a link key for 0 of 1 known device(s) (0 restored)"));
}

// ---- Where the stack drops a key ---------------------------------------------------------------

TEST_CASE("ZigbeeRejoin: a legacy device's key is held again after its authorization, not a Zigbee 3.0 device's") {
	HouseFixture f;
	hc_test::ScopedCapture capture;
	// The stack deletes the sensor's provisional key when it authorizes the legacy device.
	f.adapter.device_authorized(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kAuthorizationLegacy, 0);
	CHECK(f.driver.calls == std::vector<std::string>{"trust 0017880106498a49"});
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the trust center holds its link key "
								"again after its authorization (the well-known key it joined with): its trust-center "
								"rejoin is accepted"));
	// A failed legacy authorization, an R21 device's key exchange and an unknown device: nothing.
	f.driver.calls.clear();
	f.adapter.device_authorized(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kAuthorizationLegacy, 1);
	f.adapter.device_authorized(ZigbeeFixture::node(kPlug, kPlugShort), 1, 0);
	f.adapter.device_authorized(ZigbeeFixture::node(0x00124b00aabbccddull, 0x4242), ZigbeeAdapter::kAuthorizationLegacy,
		0);
	CHECK(f.driver.calls.empty());
}

TEST_CASE("ZigbeeRejoin: a leave the trust center hears of holds the key again; a refused rejoin is a warning") {
	HouseFixture f;
	hc_test::ScopedCapture capture;
	// The plug reports the sensor left (the trust center forgets it, the sensor comes back by a rejoin).
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateLeft, 0, kPlugShort);
	CHECK(f.driver.has("trust 0017880106498a49"));
	CHECK(capture.sink.contains("Bewegungsmelder Hof): the trust center holds its link key again after its leave"));
	// The coordinator's own child leaving to rejoin.
	f.driver.calls.clear();
	f.adapter.device_left_to_rejoin(ZigbeeFixture::node(kBulb, kBulbShort)); // unknown here: logged only
	CHECK(f.driver.calls.empty());
	f.adapter.device_left_to_rejoin(ZigbeeFixture::node(kSml002, kSml002Short));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) (short 0xaf9e) left to rejoin"));
	CHECK(f.driver.calls == std::vector<std::string>{"trust 0017880106498a49"});
	CHECK_FALSE(f.adapter.devices().find(kSml002)->left);
	// A trust-center rejoin the trust center refused is a warning, an accepted one info.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateTrustCenterRejoin, 1,
		kPlugShort);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): trust center rejoin (short 0xaf9e) "
								"through 001788010f069a57 (Schaltsteckdose Zigbee), trust center: deny"));
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateTrustCenterRejoin,
		ZigbeeAdapter::kTcAuthorize, kPlugShort);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): trust center rejoin (short 0xaf9e) "
								"through 001788010f069a57 (Schaltsteckdose Zigbee), trust center: authorize"));
}

// ---- The forced loss: Mgmt_Leave with rejoin --------------------------------------------------

TEST_CASE("ZigbeeRejoin: a requested rejoin is sent, answered and ends with the device's return") {
	HouseFixture f;
	hc_test::ScopedCapture capture;
	const char* error = nullptr;
	REQUIRE(f.adapter.request_rejoin(kSml002, &error));
	CHECK(f.driver.calls.back() == "leave+rejoin 0xaf9e");
	CHECK(f.adapter.rejoin_pending() == kSml002);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) (short 0xaf9e) asked to leave and "
								"rejoin (ZDO Mgmt_Leave with rejoin; a sleepy device takes it from its parent when it "
								"next polls)"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"rejoin\":{\"requested_ago_s\":0,\"sent\":1,\"accepted\":false}"));
	CHECK_FALSE(contains(view_of(f.adapter, kPlug), "\"rejoin\""));

	// The answer comes by the short address alone (the stack may have forgotten the IEEE by then).
	f.run_for(4000);
	f.adapter.leave_answered(ZigbeeFixture::by_short(kSml002Short), ZigbeeAdapter::kLeaveSuccess);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) took the request to leave and rejoin"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"rejoin\":{\"requested_ago_s\":4,\"sent\":1,\"accepted\":true}"));

	// It rejoins through the plug with a new address: the announce ends the request.
	f.run_for(31000);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0x5025), 0x80);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) came back 35 s after the requested "
								"rejoin (short 0x5025; requested 1 time(s))"));
	CHECK(f.adapter.rejoin_pending() == 0);
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"rejoin\""));
	CHECK(f.driver.count("leave+rejoin") == 1);
	// The wait's timer is gone: nothing is logged an hour later.
	f.run_for(ZigbeeAdapter::kRejoinWaitMs);
	CHECK_FALSE(capture.sink.contains("did not come back"));
}

TEST_CASE("ZigbeeRejoin: a request the sleepy device did not take is sent again when it is heard, a bounded number of times") {
	HouseFixture f;
	hc_test::ScopedCapture capture;
	REQUIRE(f.adapter.request_rejoin(kSml002));
	// The request expired in the plug's queue.
	f.adapter.leave_answered(ZigbeeFixture::node(kSml002, kSml002Short), 0x85);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) did not take the request to leave and "
								"rejoin (status 0x85): asked again when it is next heard"));
	// Heard sooner than kRejoinResendMs after the send: not yet.
	f.run_for(ZigbeeAdapter::kRejoinResendMs - 1000);
	report_temperature(f);
	CHECK(f.driver.count("leave+rejoin") == 1);
	// Heard later: sent again, until kRejoinSends requests went out.
	for (uint8_t n = 2; n <= ZigbeeAdapter::kRejoinSends + 2; ++n) {
		f.run_for(ZigbeeAdapter::kRejoinResendMs);
		report_temperature(f);
	}
	CHECK(f.driver.count("leave+rejoin") == ZigbeeAdapter::kRejoinSends);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) heard: asked again to leave and rejoin "
								"(request 2 of 5)"));
	f.adapter.leave_answered(ZigbeeFixture::by_short(kSml002Short), 0x85);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) did not take the request to leave and "
								"rejoin (status 0x85), requested 5 times"));
	// Nobody comes back: the wait ends with a warning and the request is gone.
	f.run_for(ZigbeeAdapter::kRejoinWaitMs);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) did not come back within 60 min of the "
								"requested rejoin (requested 5 time(s); no answer that it took the request)"));
	CHECK(f.adapter.rejoin_pending() == 0);
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"rejoin\""));
}

TEST_CASE("ZigbeeRejoin: a taken request is not sent again; an announce without the answer ends it too") {
	HouseFixture f;
	hc_test::ScopedCapture capture;
	REQUIRE(f.adapter.request_rejoin(kSml002));
	f.adapter.leave_answered(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kLeaveSuccess);
	f.run_for(ZigbeeAdapter::kRejoinResendMs * 2);
	report_temperature(f);
	CHECK(f.driver.count("leave+rejoin") == 1);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80);
	CHECK(f.adapter.rejoin_pending() == 0);

	// The answer lost: the announce still ends the request, and says so.
	REQUIRE(f.adapter.request_rejoin(kSml002));
	f.run_for(9000);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80);
	CHECK(capture.sink.contains("came back 9 s after the requested rejoin (short 0xaf9e; requested 1 time(s), its answer "
								"was not heard)"));
}

TEST_CASE("ZigbeeRejoin: refusals of a requested rejoin, each with its reason") {
	ZigbeeFixture f;
	REQUIRE(f.adapter.start());
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.request_rejoin(kSml002, &error));
	CHECK(std::string(error) == "the Zigbee network is not up");
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	join_plug(f);
	join_sml002(f);
	CHECK_FALSE(f.adapter.request_rejoin(0x00124b00aabbccddull, &error));
	CHECK(std::string(error) == "no such device");
	// The stack refuses: nothing is pending.
	f.driver.accept = false;
	CHECK_FALSE(f.adapter.request_rejoin(kSml002, &error));
	CHECK(std::string(error) == "the Zigbee stack refused the leave request");
	CHECK(f.adapter.rejoin_pending() == 0);
	f.driver.accept = true;
	// One device at a time; the same device again starts over.
	REQUIRE(f.adapter.request_rejoin(kSml002, &error));
	CHECK_FALSE(f.adapter.request_rejoin(kPlug, &error));
	CHECK(std::string(error) == "the requested rejoin of another device is pending");
	f.run_for(5000);
	REQUIRE(f.adapter.request_rejoin(kSml002, &error));
	CHECK(contains(view_of(f.adapter, kSml002), "\"rejoin\":{\"requested_ago_s\":0,\"sent\":1"));
	// Removing the device ends its request; a later leave answer is an ordinary one.
	hc_test::ScopedCapture capture;
	REQUIRE(f.adapter.remove(kSml002, &error));
	CHECK(f.adapter.rejoin_pending() == 0);
	f.adapter.leave_answered(ZigbeeFixture::by_short(kSml002Short), ZigbeeAdapter::kLeaveSuccess);
	CHECK(capture.sink.contains("I hc.zigbee: an unknown device (short 0xaf9e) answered the leave request: status 0x00"));
	// A device that left is no longer in the network to be asked.
	f.adapter.device_left(ZigbeeFixture::node(kPlug, kPlugShort));
	CHECK_FALSE(f.adapter.request_rejoin(kPlug, &error));
	CHECK(std::string(error) == "the device left the network");
}

TEST_CASE("ZigbeeRejoin: the forced loss through the plug, as the house controller logs it") {
	HouseFixture f;
	f.driver.keys = {kPlug};
	hc_test::ScopedCapture capture;
	REQUIRE(f.adapter.request_rejoin(kSml002));
	f.adapter.leave_answered(ZigbeeFixture::by_short(kSml002Short), ZigbeeAdapter::kLeaveSuccess);
	// The plug tells the trust center the sensor left (it forgets the key), then the sensor's
	// trust-center rejoin through the plug, with a new address, and its announce.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateLeft, 0, kPlugShort);
	CHECK(f.driver.has("trust 0017880106498a49"));
	f.run_for(2000);
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, 0x9061), ZigbeeAdapter::kUpdateTrustCenterRejoin,
		ZigbeeAdapter::kTcAuthorize, kPlugShort);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0x9061), 0x80);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) rejoined with a new address: short "
								"0xaf9e -> 0x9061 through 001788010f069a57 (Schaltsteckdose Zigbee), after the trust "
								"center's trust center rejoin notice"));
	CHECK(capture.sink.contains("came back 2 s after the requested rejoin (short 0x9061; requested 1 time(s))"));
	// 15 s later the stack authorizes the legacy device and drops its key: held again.
	CHECK(f.driver.count("leave+rejoin") == 1);
	f.driver.keys.clear();
	f.run_for(15000);
	f.driver.calls.clear();
	f.adapter.device_authorized(ZigbeeFixture::node(kSml002, 0x9061), ZigbeeAdapter::kAuthorizationLegacy, 0);
	CHECK(f.driver.calls == std::vector<std::string>{"trust 0017880106498a49"});
	CHECK(f.adapter.rejoin_pending() == 0);
	CHECK(f.adapter.devices().find(kSml002)->short_addr == 0x9061);
	CHECK_FALSE(capture.sink.contains("W hc.zigbee"));
}

// ---- A device that stops being known loses its key (review of #99) ------------------------------

TEST_CASE("ZigbeeRejoin: a removed device whose leave failed has no key, its trust-center rejoin is refused, the start restores none") {
	HouseFixture f;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull); // the start restores the sensor's key
	REQUIRE(f.driver.tc_rejoin_accepted(kSml002));
	hc_test::ScopedCapture capture;
	// The leave fails: the stack does not send it, the device never hears of it.
	f.driver.refuse_leave = true;
	f.driver.calls.clear();
	REQUIRE(f.adapter.remove(kSml002));
	REQUIRE(f.driver.calls.size() >= 2);
	CHECK(f.driver.calls[0] == "forget 0017880106498a49");
	CHECK(f.driver.calls[1] == "leave 0xaf9e");
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) not asked to leave (no address or "
								"the stack refused)"));
	// The trust center holds no key for it; its trust-center rejoin is refused.
	for (hc::zigbee::Ieee key : f.driver.keys) {
		CHECK(key != kSml002);
	}
	CHECK_FALSE(f.driver.tc_rejoin_accepted(kSml002));
	CHECK(f.driver.tc_rejoin_accepted(kPlug));
	// The next start restores no key for it.
	f.driver.calls.clear();
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK_FALSE(f.driver.has("trust 0017880106498a49"));
	CHECK_FALSE(f.driver.tc_rejoin_accepted(kSml002));
	CHECK(capture.sink.contains("holds a link key for 1 of 1 known device(s) (0 restored)"));
}

TEST_CASE("ZigbeeRejoin: a removed device without a short address loses its key too") {
	HouseFixture f;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	// A device the trust center told of but that never announced itself: no short address known.
	f.adapter.device_updated(ZigbeeFixture::node(0x00124b00aabbccddull, hc::zigbee::kNoShortAddress),
		ZigbeeAdapter::kUpdateSecuredRejoin, 0, kPlugShort);
	f.adapter.device_announced(ZigbeeFixture::node(0x00124b00aabbccddull, 0x4242), 0x80);
	hc::zigbee::ZigbeeDevice* device = const_cast<hc::zigbee::DeviceDatabase&>(f.adapter.devices()).find(
		0x00124b00aabbccddull);
	REQUIRE(device != nullptr);
	device->short_addr = hc::zigbee::kNoShortAddress;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	REQUIRE(f.driver.tc_rejoin_accepted(0x00124b00aabbccddull));
	hc_test::ScopedCapture capture;
	f.driver.calls.clear();
	REQUIRE(f.adapter.remove(0x00124b00aabbccddull));
	CHECK(f.driver.calls == std::vector<std::string>{"forget 00124b00aabbccdd"});
	CHECK(capture.sink.contains("not asked to leave (no address or the stack refused)"));
	CHECK_FALSE(f.driver.tc_rejoin_accepted(0x00124b00aabbccddull));
}

TEST_CASE("ZigbeeRejoin: a key the stack cannot drop refuses the removal with the reason, nothing changes") {
	HouseFixture f;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	hc_test::ScopedCapture capture;
	f.driver.accept = false;
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.remove(kSml002, &error));
	CHECK(std::string(error) == "the trust center's link key for the device could not be dropped (try again when the "
								"network is up)");
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) not removed: the trust center's link "
								"key for it could not be dropped (the stack does not run or kept it); try again when the "
								"network is up"));
	CHECK(f.adapter.devices().find(kSml002) != nullptr);
	CHECK_FALSE(f.driver.has("leave 0xaf9e"));
	// Once the stack takes it, the removal goes through.
	f.driver.accept = true;
	REQUIRE(f.adapter.remove(kSml002, &error));
	CHECK_FALSE(f.driver.tc_rejoin_accepted(kSml002));
}

TEST_CASE("ZigbeeRejoin: a device that left without rejoin loses its key; one the stack keeps is a warning") {
	HouseFixture f;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	hc_test::ScopedCapture capture;
	f.adapter.device_left(ZigbeeFixture::node(kSml002, kSml002Short));
	CHECK(f.driver.has("forget 0017880106498a49"));
	CHECK_FALSE(f.driver.tc_rejoin_accepted(kSml002));
	f.driver.accept = false;
	f.adapter.device_left(ZigbeeFixture::node(kPlug, kPlugShort));
	CHECK(capture.sink.contains("W hc.zigbee: 001788010f069a57 (Schaltsteckdose Zigbee): the trust center's link key for "
								"it could not be dropped: its trust-center rejoin is still accepted"));
}

// ---- The key table's answers (the house controller, v1.5.0-14-gf19ec7b) ----------------------

TEST_CASE("ZigbeeRejoin: the stack's inverted answer to a written key is no refusal; the table decides") {
	// The house controller's boot (f19ec7b): "the trust center holds no link key for it and did not take
	// one at the network's start": esp_zb_secur_update_key_pair() answered ESP_FAIL for a key it wrote.
	HouseFixture f;
	f.driver.keys = {kPlug};
	hc_test::ScopedCapture capture;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK_FALSE(capture.sink.contains("did not take one"));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the trust center holds its link key "
								"again at the network's start"));
	CHECK(capture.sink.contains("holds a link key for 2 of 2 known device(s) (1 restored)"));
	CHECK(f.driver.tc_rejoin_accepted(kSml002));
}

TEST_CASE("ZigbeeRejoin: a write the stack answers with success but does not keep is a refusal with that answer") {
	HouseFixture f;
	f.driver.keys = {kPlug};
	f.driver.table_full = true; // the key pair table takes nothing; the wrapper answers ESP_OK (0)
	hc_test::ScopedCapture capture;
	f.adapter.network_state(true, 0x1a62, 20, 0xddddddddddddddddull);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the trust center holds no link key "
								"for it and did not take one at the network's start (stack answer 0): its trust-center "
								"rejoin is refused"));
	CHECK(capture.sink.contains("holds a link key for 1 of 2 known device(s) (0 restored)"));
	CHECK_FALSE(f.driver.tc_rejoin_accepted(kSml002));
}

TEST_CASE("ZigbeeRejoin: restore_key and forget_key judge by the table alone") {
	struct Table : hc::zigbee::KeyTable {
		bool present = false;
		bool writes = true;
		int answer = 0;
		int adds = 0;
		bool has_key(hc::zigbee::Ieee) override { return present; }
		int add_standard_key(hc::zigbee::Ieee) override {
			++adds;
			present = present || writes;
			return answer;
		}
		void delete_keys(hc::zigbee::Ieee) override { present = !writes; }
	} table;
	int answer = 99;
	table.present = true;
	CHECK(hc::zigbee::restore_key(table, kSml002, answer) == hc::ZigbeeDriver::Trust::kept);
	CHECK(answer == 0);
	CHECK(table.adds == 0);
	table.present = false;
	table.answer = -1; // ESP_FAIL for a written key
	CHECK(hc::zigbee::restore_key(table, kSml002, answer) == hc::ZigbeeDriver::Trust::restored);
	CHECK(answer == -1);
	table.present = false;
	table.writes = false;
	table.answer = 0; // ESP_OK for a key not written
	CHECK(hc::zigbee::restore_key(table, kSml002, answer) == hc::ZigbeeDriver::Trust::refused);
	CHECK(answer == 0);
	table.present = true;
	CHECK_FALSE(hc::zigbee::forget_key(table, kSml002)); // the delete kept it
	table.writes = true;
	CHECK(hc::zigbee::forget_key(table, kSml002));
}
