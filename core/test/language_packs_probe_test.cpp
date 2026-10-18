/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the language packs (issue #24), written against doc/i18n.md: a device set to a regional
// tag ("de-AT") when the release ships only "de", when it ships both, and when the base entry is
// unusable; "en-GB" needs no pack.

#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/language_packs.h"
#include "hc/network_settings.h"
#include "fake_fetch.h"
#include "hc/sha.h"
#include "web_test_support.h"

using hc::LanguagePacks;
using hc::LanguagePackState;

namespace {

class OneFetcher : public hc::HttpFetcher {
public:
	bool fetch(const char* url, hc::FetchReceiver& receiver, const char** error) override {
		if (receiver_ != nullptr) {
			*error = "busy";
			return false;
		}
		urls.push_back(url);
		receiver_ = &receiver;
		return true;
	}
	bool busy() const override { return receiver_ != nullptr; }
	void ok(const std::string& body) {
		REQUIRE(receiver_ != nullptr);
		hc_test::respond(*receiver_, 200, body);
		receiver_ = nullptr;
	}
	std::vector<std::string> urls;

private:
	hc::FetchReceiver* receiver_ = nullptr;
};

std::string sha(const std::string& data) {
	uint8_t digest[hc::Sha256::kDigestSize];
	hc::Sha256::hash(data.data(), data.size(), digest);
	char hex[65];
	hc::to_hex(digest, sizeof(digest), hex);
	return hex;
}

std::string entry(const std::string& code, const std::string& path, const std::string& body, const std::string& size = "") {
	return "\"" + code + "\": {\"name\": \"" + code + "\", \"path\": \"" + path + "\", \"sha256\": \"" + sha(body) +
		"\", \"size\": " + (size.empty() ? std::to_string(body.size()) : size) + "}";
}

const std::string kDe = std::string("\x1f\x8b\x08\x00", 4) + "{\"a\":\"de\"}";
const std::string kDeAt = std::string("\x1f\x8b\x08\x00", 4) + "{\"a\":\"de-AT\"}";

struct Probe {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	hc_test::FakeTimeZone zone;
	hc_test::FakeSettings settings;
	hc::TimeService time{loop, clock, zone, settings};
	OneFetcher fetcher;
	hc_test::FakeBlobStore store;
	LanguagePacks packs{loop, time, settings, connectivity, fetcher, store, "0.1.0-dev"};

	explicit Probe(const char* lang) {
		connectivity.start();
		clock.wall_seconds = 1792270800;
		clock.synchronized = true;
		connectivity.link_up("192.0.2.100");
		settings.set_string(hc::setting::kUpdateUrl, "https://updates.example/hc");
		settings.set_string(hc::setting::kUiLang, lang);
		packs.start();
		run_for(LanguagePacks::kStartDelayMs + 1);
	}

	void run_for(uint64_t ms) {
		uint64_t until = clock.now_ms + ms;
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until) {
				clock.now_ms = until;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	bool stored(const char* name) const { return store.blobs.find(name) != store.blobs.end(); }
};

} // namespace

TEST_CASE("Lang probe: de-AT takes the release's de pack, stored under its own tag") {
	Probe p("de-AT");
	REQUIRE(p.fetcher.urls.size() == 1);
	CHECK(p.fetcher.urls[0] == "https://updates.example/hc/0.1.0-dev/i18n/manifest.json");
	p.fetcher.ok("{\"version\": \"0.1.0-dev\", \"packs\": {" + entry("de", "lang-de.json.gz", kDe) + "}}");
	p.run_for(1);
	REQUIRE(p.fetcher.urls.size() == 2);
	CHECK(p.fetcher.urls[1] == "https://updates.example/hc/0.1.0-dev/i18n/lang-de.json.gz");
	p.fetcher.ok(kDe);
	CHECK(p.packs.state() == LanguagePackState::installed);
	CHECK(p.stored("i18n.de-AT.json.gz")); // what GET /i18n/de-AT.json serves
	CHECK_FALSE(p.stored("i18n.de.json.gz"));
	CHECK(p.packs.offer_count() == 1);
}

TEST_CASE("Lang probe: de-AT prefers the release's own de-AT pack over de, whatever the order") {
	for (int order = 0; order < 2; ++order) {
		Probe p("de-AT");
		std::string de = entry("de", "lang-de.json.gz", kDe);
		std::string at = entry("de-AT", "lang-de-AT.json.gz", kDeAt);
		p.fetcher.ok("{\"version\": \"0.1.0-dev\", \"packs\": {" + (order == 0 ? de + ", " + at : at + ", " + de) + "}}");
		p.run_for(1);
		REQUIRE(p.fetcher.urls.size() == 2);
		CHECK(p.fetcher.urls[1] == "https://updates.example/hc/0.1.0-dev/i18n/lang-de-AT.json.gz");
		p.fetcher.ok(kDeAt);
		CHECK(p.packs.state() == LanguagePackState::installed);
		CHECK(p.packs.offer_count() == 2);
	}
}

TEST_CASE("Lang probe: an unusable de entry fails a de-AT device instead of reporting not offered") {
	Probe p("de-AT");
	p.fetcher.ok("{\"version\": \"0.1.0-dev\", \"packs\": {" + entry("de", "lang-de.json.gz", kDe, "0") + "}}");
	p.run_for(1);
	CHECK(p.fetcher.urls.size() == 1);
	CHECK(p.packs.state() == LanguagePackState::failed);
	CHECK(std::string(p.packs.detail()).find("size") != std::string::npos);
}

TEST_CASE("Lang probe: en-GB is the built-in language, a stored pack is removed") {
	hc_test::FakeBlobStore* store = nullptr;
	Probe p("en-GB");
	store = &p.store;
	store->blobs["i18n.en-GB.json.gz"] = {'x'};
	p.fetcher.ok("{\"version\": \"0.1.0-dev\", \"packs\": {" + entry("de", "lang-de.json.gz", kDe) + "}}");
	p.run_for(1);
	CHECK(p.fetcher.urls.size() == 1);
	CHECK(p.packs.state() == LanguagePackState::builtin);
	CHECK_FALSE(p.stored("i18n.en-GB.json.gz"));
}
