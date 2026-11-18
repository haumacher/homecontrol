/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The network name (homecontrol #89, doc/network.md "Network name"): the rules of the name (the
// phoneblock dongle's, ported), the first pick that stores it, the stored name that is never picked
// again, the user's change with its lookup, and the setting net.hostname in the web API.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/network_name.h"
#include "hc/network_settings.h"
#include "hc/platform/name_service.h"
#include "fake_name_service.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::NetworkName;
using hc::setting::kMaxNetHostnameLength;
using hc_test::FakeClock;
using hc_test::FakeNameService;
using hc_test::Json;

namespace {

struct Fixture {
	hc_test::ScopedCapture capture;
	FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	hc_test::FakeSettings settings;
	FakeNameService service{clock};

	/// Runs the loop until `until_ms`.
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

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	std::string stored() const {
		auto it = settings.entries.find(hc::setting::kNetHostname);
		return it == settings.entries.end() ? std::string("<none>") : it->second.text;
	}
};

/// normalize_network_name() accepts `input` as `expected`.
void accepts(const char* input, const char* expected) {
	char out[kMaxNetHostnameLength + 1] = "garbage";
	const char* error = nullptr;
	CHECK_MESSAGE(hc::normalize_network_name(input, out, &error), input);
	CHECK(std::string(out) == expected);
}

/// normalize_network_name() refuses `input`, with an empty result and a reason containing `reason`.
void refuses(const char* input, const char* reason) {
	char out[kMaxNetHostnameLength + 1] = "garbage";
	const char* error = nullptr;
	CHECK_FALSE_MESSAGE(hc::normalize_network_name(input, out, &error), (input != nullptr ? input : "(null)"));
	CHECK(out[0] == '\0');
	REQUIRE(error != nullptr);
	CHECK_MESSAGE(std::string(error).find(reason) != std::string::npos, error);
}

} // namespace

// ---- The rules ----------------------------------------------------------------------------------

TEST_CASE("NetworkName.TheDonglesRulesNormalizeTheInput") {
	accepts("homecontrol", "homecontrol");
	accepts("Answerbot-S3", "answerbot-s3");       // upper case is lowered
	accepts("  keller-2\t", "keller-2");           // surrounding whitespace is dropped
	accepts("\r\nKITCHEN\n", "kitchen");
	accepts("a", "a");                             // one character is enough
	accepts("0", "0");                             // a digit alone is a label
	accepts("abcdefghijklmnopqrstuvwxyz012345", "abcdefghijklmnopqrstuvwxyz012345"); // 32 characters
	refuses("abcdefghijklmnopqrstuvwxyz0123456", "longer than 32"); // 33 characters
	refuses("-x", "start or end with '-'");
	refuses("x-", "start or end with '-'");
	refuses("a_b", "only letters a-z, digits and '-'");  // underscore
	refuses("a.b", "only letters a-z, digits and '-'");  // dot: one label only
	refuses("\xc3\xa4", "only letters a-z, digits and '-'"); // non-ASCII (an umlaut)
	refuses("a b", "only letters a-z, digits and '-'");  // inner space
	// Empty is not "back to the default" as on the dongle: the name is picked once and then the user's.
	refuses("", "a name is required");
	refuses("   ", "a name is required");
	refuses(nullptr, "a name is required");
}

TEST_CASE("NetworkName.TheStoredFormIsAlreadyNormalized") {
	CHECK(hc::network_name_valid(hc::setting::kDefaultNetHostname));
	CHECK(hc::network_name_valid("homecontrol-2"));
	CHECK_FALSE(hc::network_name_valid("Homecontrol")); // stored values are lower case
	CHECK_FALSE(hc::network_name_valid(" homecontrol"));
	CHECK_FALSE(hc::network_name_valid(""));
	CHECK_FALSE(hc::network_name_valid(nullptr));
}

TEST_CASE("NetworkName.StoreValidatesAndReadRefusesWhatCannotBeUsed") {
	hc_test::ScopedCapture capture;
	hc_test::FakeSettings settings;
	char name[kMaxNetHostnameLength + 1] = "garbage";
	CHECK_FALSE(hc::read_network_name(settings, name, "test"));
	CHECK(name[0] == '\0');
	CHECK_FALSE(capture.sink.contains("net.hostname")); // nothing stored is not an error

	const char* error = nullptr;
	CHECK_FALSE(hc::store_network_name(settings, "Kitchen", &error));
	CHECK(settings.entries.count("net.hostname") == 0);
	CHECK(hc::store_network_name(settings, "kitchen", &error));
	CHECK(hc::read_network_name(settings, name, "test"));
	CHECK(std::string(name) == "kitchen");

	// A value nobody may store (written by hand): logged, and it counts as none.
	settings.set_string("net.hostname", "Bad_Name");
	CHECK_FALSE(hc::read_network_name(settings, name, "test"));
	CHECK(capture.sink.contains("E test: setting net.hostname is invalid"));
	// Too long for the name, but it fits a setting: refused, not cut to 32 characters.
	settings.set_string("net.hostname", "abcdefghijklmnopqrstuvwxyz0123456789");
	CHECK_FALSE(hc::read_network_name(settings, name, "test"));
	CHECK(name[0] == '\0');
}

// ---- The first pick -----------------------------------------------------------------------------

TEST_CASE("NetworkName.TheFirstPickAnnouncesHomecontrolAndStoresIt") {
	Fixture f;
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	REQUIRE(name.start(f.connectivity));
	CHECK(std::string(name.name()) == "homecontrol"); // the first DHCP host name
	CHECK_FALSE(name.stored());
	CHECK(f.service.lookups.empty()); // nothing before the network is up

	f.connectivity.link_up("192.0.2.10");
	CHECK(f.service.lookups == std::vector<std::string>{"homecontrol"});
	CHECK(f.service.timeouts == std::vector<uint32_t>{NetworkName::kLookupMs});
	CHECK(name.picking());
	f.run_for(200);
	CHECK(f.service.applied == std::vector<std::string>{"homecontrol"});
	CHECK(f.stored() == "<none>"); // not before probing had its time
	f.run_for(NetworkName::kSettleMs);
	CHECK(f.stored() == "homecontrol");
	CHECK(name.stored());
	CHECK_FALSE(name.picking());
	CHECK(std::string(name.announced()) == "homecontrol");
	CHECK(f.capture.sink.contains("I hc.name: network name homecontrol picked and stored (net.hostname)"));

	// A later loss and return of the link neither picks nor announces again.
	f.connectivity.link_down("gone");
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs * 2);
	CHECK(f.service.lookups.size() == 1);
	CHECK(f.service.applied.size() == 1);
}

TEST_CASE("NetworkName.ATakenNameMakesItHomecontrol2WhichStaysAfterARestartWithTheOtherHostGone") {
	Fixture f;
	f.service.taken = {"homecontrol"};
	{
		NetworkName name(f.loop, f.clock, f.settings, f.service);
		name.start(f.connectivity);
		f.connectivity.link_up("192.0.2.10");
		f.run_for(NetworkName::kSettleMs + 1000);
		CHECK(f.service.lookups == std::vector<std::string>{"homecontrol", "homecontrol-2"});
		CHECK(f.service.applied == std::vector<std::string>{"homecontrol-2"});
		CHECK(f.stored() == "homecontrol-2");
		CHECK(f.capture.sink.contains("I hc.name: homecontrol.local is taken on this network: trying the next name"));
	}

	// The restart: the other host is gone, the stored name is announced and nothing is asked.
	Fixture g;
	g.settings = f.settings;
	NetworkName again(g.loop, g.clock, g.settings, g.service);
	again.start(g.connectivity);
	CHECK(std::string(again.name()) == "homecontrol-2");
	CHECK(again.stored());
	g.connectivity.link_up("192.0.2.10");
	CHECK(g.service.lookups.empty());
	CHECK(g.service.applied == std::vector<std::string>{"homecontrol-2"});
	g.run_for(NetworkName::kSettleMs + 1000);
	CHECK(g.stored() == "homecontrol-2");
}

TEST_CASE("NetworkName.ARouterEchoingTheControllersOwnDhcpNameDoesNotTakeIt") {
	// On the desk (2026-11-18): the FRITZ!Box answers mDNS for the host names of its DHCP clients, so
	// "homecontrol.local" came back with the controller's own address (the DHCP host name it sent
	// before the pick) and the controller stored homecontrol-2 with no other host on the LAN.
	Fixture f;
	f.service.echoes = {{"homecontrol", "192.0.2.10"}};
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.service.lookups == std::vector<std::string>{"homecontrol"});
	CHECK(f.service.own_addresses == std::vector<std::string>{"192.0.2.10"});
	CHECK(f.service.applied == std::vector<std::string>{"homecontrol"});
	CHECK(f.stored() == "homecontrol");
}

TEST_CASE("NetworkName.ARouterAnsweringWithAnotherAddressTakesTheName") {
	Fixture f;
	f.service.echoes = {{"homecontrol", "192.0.2.77"}};
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.service.lookups == std::vector<std::string>{"homecontrol", "homecontrol-2"});
	CHECK(f.stored() == "homecontrol-2");
}

TEST_CASE("NetworkName.TheNameProbingEndsUpWithIsStored") {
	// Two controllers picking at the same moment: both found homecontrol free, the responder of one
	// renamed it while probing.
	Fixture f;
	f.service.probe_renames = {{"homecontrol", "homecontrol-2"}};
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.stored() == "homecontrol-2");
	CHECK(std::string(name.name()) == "homecontrol-2");
}

TEST_CASE("NetworkName.WhenAllCandidatesAreTakenTheLastIsAnnouncedAndProbingDecides") {
	Fixture f;
	for (unsigned i = 1; i <= NetworkName::kMaxCandidates; ++i) {
		f.service.taken.insert(i == 1 ? std::string("homecontrol") : "homecontrol-" + std::to_string(i));
	}
	f.service.probe_renames = {{"homecontrol-9", "homecontrol-10"}};
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 2000);
	CHECK(f.service.lookups.size() == NetworkName::kMaxCandidates);
	CHECK(f.service.applied == std::vector<std::string>{"homecontrol-9"});
	CHECK(f.stored() == "homecontrol-10");
	CHECK(f.capture.sink.contains("W hc.name: homecontrol-9.local and the names before it are taken"));
}

TEST_CASE("NetworkName.ALookupThatCannotStartOrHasNoOutcomeLeavesTheChoiceToProbing") {
	SUBCASE("cannot start") {
		Fixture f;
		f.service.lookups_start = false;
		NetworkName name(f.loop, f.clock, f.settings, f.service);
		name.start(f.connectivity);
		f.connectivity.link_up("192.0.2.10");
		CHECK(f.service.applied == std::vector<std::string>{"homecontrol"});
		CHECK(f.capture.sink.contains("W hc.name: cannot ask whether homecontrol is free (mDNS is not running)"));
		f.run_for(NetworkName::kSettleMs);
		CHECK(f.stored() == "homecontrol");
	}
	SUBCASE("no outcome") {
		Fixture f;
		f.service.hang = true;
		NetworkName name(f.loop, f.clock, f.settings, f.service);
		name.start(f.connectivity);
		f.connectivity.link_up("192.0.2.10");
		f.run_for(NetworkName::kLookupMs);
		CHECK(f.service.applied.empty());
		f.run_for(NetworkName::kLookupGraceMs + NetworkName::kPollMs);
		CHECK(f.service.cancels == 1);
		CHECK(f.service.applied == std::vector<std::string>{"homecontrol"});
		CHECK(f.capture.sink.contains("had no outcome after 3000 ms: given up"));
	}
}

TEST_CASE("NetworkName.AnAnnouncementThatFailsIsRetriedAtTheNextNetworkUp") {
	Fixture f;
	f.service.apply_ok = false;
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.stored() == "<none>");
	CHECK(f.capture.sink.contains("E hc.name: network name homecontrol not announced: mdns_init failed (retried at the "
		"next network up)"));
	f.service.apply_ok = true;
	f.connectivity.link_down("gone");
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.service.applied == std::vector<std::string>{"homecontrol"});
	CHECK(f.stored() == "homecontrol");
}

TEST_CASE("NetworkName.AStoredNameTakenWhileTheControllerWasOffStaysStored") {
	Fixture f;
	f.settings.set_string("net.hostname", "homecontrol");
	f.service.probe_renames = {{"homecontrol", "homecontrol-2"}};
	NetworkName name(f.loop, f.clock, f.settings, f.service);
	name.start(f.connectivity);
	f.connectivity.link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.service.lookups.empty());
	CHECK(f.stored() == "homecontrol");                // the user's (or the first pick's) choice
	CHECK(std::string(name.announced()) == "homecontrol-2"); // what the LAN reaches for this run
	CHECK(f.capture.sink.contains("W hc.name: homecontrol.local is taken on this network: announcing "
		"homecontrol-2.local until the restart (net.hostname stays homecontrol)"));
}

// ---- The user's change ------------------------------------------------------------------------

namespace {

struct Renamed : Fixture {
	NetworkName name{loop, clock, settings, service};

	Renamed() {
		settings.set_string("net.hostname", "homecontrol");
		name.start(connectivity);
		connectivity.link_up("192.0.2.10");
		run_for(NetworkName::kSettleMs + 1000);
		service.applied.clear();
	}

	/// begin_rename() and, when it checks, the loop until the outcome.
	NetworkName::Begin rename(const char* input, bool& ok, std::string& code, std::string& error) {
		const char* c = "";
		const char* e = "";
		NetworkName::Begin begin = name.begin_rename(input, &c, &e);
		ok = begin == NetworkName::Begin::done;
		code = c;
		error = e;
		if (begin != NetworkName::Begin::checking) {
			return begin;
		}
		for (int i = 0; i < 100; ++i) {
			if (name.rename_finished(ok, &c, &e)) {
				code = c;
				error = e;
				return begin;
			}
			run_for(NetworkName::kPollMs);
		}
		FAIL("the check never finished");
		return begin;
	}
};

} // namespace

TEST_CASE("NetworkName.AFreeNameIsStoredAndAnnouncedAtOnce") {
	Renamed f;
	int checked = 0;
	struct Counter {
		int* count;
		void hit() { ++*count; }
	} counter{&checked};
	f.name.set_rename_listener(hc::Callback::member<Counter, &Counter::hit>(&counter));
	bool ok = false;
	std::string code, error;
	CHECK(f.rename("  Kitchen ", ok, code, error) == NetworkName::Begin::checking);
	CHECK(ok);
	CHECK(checked == 1);
	CHECK(f.service.lookups.back() == "kitchen");
	CHECK(f.stored() == "kitchen");
	CHECK(f.service.applied == std::vector<std::string>{"kitchen"}); // mDNS and the DHCP host name
	CHECK(std::string(f.name.announced()) == "kitchen");
	CHECK(f.capture.sink.contains("I hc.name: network name changed to kitchen (was homecontrol)"));
	// The outcome is handed out once.
	bool again = true;
	const char* c = "";
	const char* e = "";
	CHECK(f.name.rename_finished(again, &c, &e));
	CHECK_FALSE(again);
	CHECK(std::string(c) == "api.not_deferred");
}

TEST_CASE("NetworkName.ANameAnotherHostAnswersForIsRefusedAndNothingChanges") {
	Renamed f;
	f.service.taken = {"printer"};
	bool ok = true;
	std::string code, error;
	f.rename("printer", ok, code, error);
	CHECK_FALSE(ok);
	CHECK(code == "settings.name_taken");
	CHECK(error == "printer is already in use on this network");
	CHECK(f.stored() == "homecontrol");
	CHECK(f.service.applied.empty());
	CHECK(std::string(f.name.name()) == "homecontrol");
	CHECK(f.capture.sink.contains("W hc.name: network name printer refused: printer is already in use on this network"));
}

TEST_CASE("NetworkName.ARenameToANameAnsweredWithTheOwnAddressIsTaken") {
	// The router still answers with the controller's address for the DHCP host name it carried
	// before: that name is the controller's own, not another host's.
	Renamed f;
	f.service.echoes = {{"homecontrol-test", "192.0.2.10"}, {"printer", "192.0.2.99"}};
	bool ok = false;
	std::string code, error;
	f.rename("homecontrol-test", ok, code, error);
	CHECK(ok);
	CHECK(f.stored() == "homecontrol-test");
	f.rename("printer", ok, code, error);
	CHECK_FALSE(ok);
	CHECK(code == "settings.name_taken");
	CHECK(f.stored() == "homecontrol-test");
}

TEST_CASE("NetworkName.InvalidAndEmptyNamesAreRefusedWithoutALookup") {
	Renamed f;
	bool ok = true;
	std::string code, error;
	CHECK(f.rename("", ok, code, error) == NetworkName::Begin::refused);
	CHECK(code == "settings.invalid");
	CHECK(error == "a name is required");
	CHECK(f.rename("my_controller", ok, code, error) == NetworkName::Begin::refused);
	CHECK(code == "settings.invalid");
	CHECK(f.rename("kitchen.local", ok, code, error) == NetworkName::Begin::refused);
	CHECK(f.service.lookups.size() == 0);
	CHECK(f.stored() == "homecontrol");
}

TEST_CASE("NetworkName.ItsOwnNameIsStoredWithoutALookup") {
	Renamed f;
	bool ok = false;
	std::string code, error;
	CHECK(f.rename("HomeControl", ok, code, error) == NetworkName::Begin::done);
	CHECK(ok);
	CHECK(f.service.lookups.empty()); // it would find itself answering
	CHECK(f.stored() == "homecontrol");

	// The name probing gave it for this run (its stored name was taken): taking it is no lookup either.
	Renamed g;
	g.service.announced = "homecontrol-2";
	CHECK(g.rename("homecontrol-2", ok, code, error) == NetworkName::Begin::done);
	CHECK(g.service.lookups.empty());
	CHECK(g.stored() == "homecontrol-2");
}

TEST_CASE("NetworkName.OneCheckAtATimeAndNoneDuringTheFirstPick") {
	Renamed f;
	const char* code = "";
	const char* error = "";
	f.service.answer_ms = 500;
	CHECK(f.name.begin_rename("kitchen", &code, &error) == NetworkName::Begin::checking);
	CHECK(f.name.begin_rename("cellar", &code, &error) == NetworkName::Begin::refused);
	CHECK(std::string(code) == "settings.busy");
	f.run_for(1000);
	bool ok = false;
	CHECK(f.name.rename_finished(ok, &code, &error));
	CHECK(ok);

	Fixture g;
	NetworkName picking(g.loop, g.clock, g.settings, g.service);
	picking.start(g.connectivity);
	g.connectivity.link_up("192.0.2.10");
	CHECK(picking.begin_rename("kitchen", &code, &error) == NetworkName::Begin::refused);
	CHECK(std::string(code) == "settings.busy");
	CHECK(std::string(error).find("still choosing") != std::string::npos);
	g.run_for(NetworkName::kSettleMs + 1000);
	CHECK(picking.begin_rename("kitchen", &code, &error) == NetworkName::Begin::checking);
}

TEST_CASE("NetworkName.ALookupThatCannotRunRefusesTheChange") {
	Renamed f;
	f.service.lookups_start = false;
	bool ok = true;
	std::string code, error;
	CHECK(f.rename("kitchen", ok, code, error) == NetworkName::Begin::refused);
	CHECK(code == "settings.name_unchecked");
	CHECK(error == "cannot check whether the name is free: mDNS is not running");
	f.service.lookups_start = true;
	f.service.fail = true;
	f.rename("kitchen", ok, code, error);
	CHECK_FALSE(ok);
	CHECK(code == "settings.name_unchecked");
	CHECK(f.stored() == "homecontrol");
}

// ---- The web API --------------------------------------------------------------------------------

namespace {

struct WebName : hc_test::WebFixture {
	FakeNameService service{fake.clock};
	NetworkName name{controller.loop(), fake.clock, fake.settings, service};
	std::string cookie;

	WebName() {
		api.set_network_name(&name);
		name.start(controller.connectivity());
		cookie = onboard();
	}

	void run_for(uint64_t ms) {
		uint64_t until = fake.clock.now_ms + ms;
		for (;;) {
			controller.loop().run_once(fake.clock.now_ms);
			uint64_t deadline = controller.loop().next_deadline();
			if (deadline > until) {
				fake.clock.now_ms = until;
				controller.loop().run_once(fake.clock.now_ms);
				return;
			}
			if (deadline > fake.clock.now_ms) {
				fake.clock.now_ms = deadline;
			}
		}
	}

	/// PUT /api/settings/net.hostname, polled as the platforms poll a deferred answer.
	Result put(const std::string& value, bool* deferred = nullptr) {
		std::string body = "{\"value\":\"" + value + "\"}";
		hc::WebRequest req;
		req.method = hc::HttpMethod::put;
		req.path = "/api/settings/net.hostname";
		req.query = "";
		req.cookie = cookie.c_str();
		req.requested_with = "HomeControl";
		req.source = "192.0.2.7";
		req.body = body.c_str();
		req.body_length = body.size();
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		if (deferred != nullptr) {
			*deferred = resp.deferred;
		}
		for (int i = 0; resp.deferred && i < 100; ++i) {
			run_for(hc::WebApi::kDeferredPollMs);
			if (api.poll_deferred(resp)) {
				break;
			}
		}
		return result(resp);
	}

	Json setting() {
		auto list = get("/api/settings", cookie);
		for (const auto& s : list.json["settings"].items) {
			if (s["key"].text == "net.hostname") {
				return s;
			}
		}
		FAIL("net.hostname is not listed");
		return Json();
	}
};

} // namespace

TEST_CASE("NetworkName.WebApiListsTheSettingAndReportsTheAnnouncedName") {
	WebName f;
	Json s = f.setting();
	CHECK(s["value"].kind == Json::Kind::null); // not picked yet
	CHECK(s["default"].text == "homecontrol");
	CHECK(s["editable"].boolean);

	f.controller.connectivity().link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	CHECK(f.setting()["value"].text == "homecontrol");
	auto status = f.get("/api/status", f.cookie);
	CHECK(status.json["network"]["mdns"].text == "homecontrol.local");
	CHECK(status.json["device"]["name"].text == "homecontrol-a1b2c3"); // the identity stays

	f.service.announced = "homecontrol-2"; // probing renamed it for this run
	status = f.get("/api/status", f.cookie);
	CHECK(status.json["network"]["mdns"].text == "homecontrol-2.local");
}

TEST_CASE("NetworkName.WebApiRenamesAfterTheLookupAndRefusesWithTheReason") {
	WebName f;
	f.controller.connectivity().link_up("192.0.2.10");
	f.run_for(NetworkName::kSettleMs + 1000);
	f.service.taken = {"printer"};

	bool deferred = false;
	auto taken = f.put("printer", &deferred);
	CHECK(deferred);
	CHECK(taken.status == 422);
	CHECK(taken.json["code"].text == "settings.name_taken");
	CHECK(taken.json["error"].text == "printer is already in use on this network");
	CHECK(f.setting()["value"].text == "homecontrol");

	auto invalid = f.put("my_controller", &deferred);
	CHECK_FALSE(deferred);
	CHECK(invalid.status == 422);
	CHECK(invalid.json["code"].text == "settings.invalid");
	auto empty = f.put("  ");
	CHECK(empty.status == 422);
	CHECK(empty.json["error"].text == "a name is required");

	auto good = f.put("Kitchen", &deferred);
	CHECK(deferred);
	REQUIRE(good.status == 200);
	bool listed = false;
	for (const auto& s : good.json["settings"].items) {
		if (s["key"].text == "net.hostname") {
			listed = true;
			CHECK(s["value"].text == "kitchen");
		}
	}
	CHECK(listed);
	CHECK(f.service.applied.back() == "kitchen");
	CHECK(f.get("/api/status", f.cookie).json["network"]["mdns"].text == "kitchen.local");
	CHECK(f.get("/api/status", f.cookie).json["device"]["name"].text == "homecontrol-a1b2c3");

	auto same = f.put("kitchen", &deferred);
	CHECK_FALSE(deferred); // its own name: no lookup
	CHECK(same.status == 200);
}

TEST_CASE("NetworkName.WebApiDuringTheFirstPickAnswersBusy") {
	WebName f;
	f.controller.connectivity().link_up("192.0.2.10");
	auto busy = f.put("kitchen");
	CHECK(busy.status == 409);
	CHECK(busy.json["code"].text == "settings.busy");
}

TEST_CASE("NetworkName.WithoutANetworkNameTheSettingIsNotEditable") {
	hc_test::WebFixture f; // the POSIX build: no mDNS
	std::string cookie = f.onboard();
	auto list = f.get("/api/settings", cookie);
	bool found = false;
	for (const auto& s : list.json["settings"].items) {
		if (s["key"].text == "net.hostname") {
			found = true;
			CHECK_FALSE(s["editable"].boolean);
		}
	}
	CHECK(found);
	auto put = f.request(hc::HttpMethod::put, "/api/settings/net.hostname", "{\"value\":\"kitchen\"}", cookie);
	CHECK(put.status == 403);
	CHECK(put.json["code"].text == "settings.not_editable");
	CHECK(f.get("/api/status", cookie).json["network"]["mdns"].text == "homecontrol-a1b2c3.local");
}
