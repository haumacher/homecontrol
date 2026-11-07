/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Internationalization (issue #24, doc/i18n.md): the ui.lang setting, the language pack sync under
// the fake clock with a scripted fetcher, and the key sets of the web UI's packs.
#include <filesystem>
#include <map>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "fake_fetch.h"
#include "hc/language_packs.h"
#include "hc/network_settings.h"
#include "hc/sha.h"
#include "web_test_support.h"

#ifndef HC_WEB_DIR
#error "HC_WEB_DIR must name the web directory"
#endif

using hc::LanguagePacks;
using hc::LanguagePackState;
using hc_test::FakeBlobStore;
using hc_test::FakeClock;
using hc_test::FakeNetworkDriver;
using hc_test::FakeSettings;
using hc_test::FakeTimeZone;
using hc_test::Json;
using hc_test::JsonReader;
using hc_test::ScopedCapture;

namespace {

/// Records the fetches; the test completes them.
class ScriptedFetcher : public hc::HttpFetcher {
public:
	bool fetch(const char* url, hc::FetchReceiver& receiver, const char** error) override {
		if (receiver_ != nullptr) {
			*error = "busy: a fetch is in flight";
			return false;
		}
		urls.push_back(url);
		receiver_ = &receiver;
		return true;
	}

	bool busy() const override { return receiver_ != nullptr; }

	bool pending() const { return receiver_ != nullptr; }

	/// A 200 response with `body` in pieces of `piece` bytes (announced unless `announce` is false).
	hc::FetchResult ok(const std::string& body, size_t piece = 1024, bool announce = true) {
		REQUIRE(receiver_ != nullptr);
		hc_test::Delivery delivery;
		delivery.piece = piece;
		delivery.announce = announce;
		// Like the platforms: busy until the receiver returned.
		hc::FetchResult result = hc_test::respond(*receiver_, 200, body, delivery);
		receiver_ = nullptr;
		pieces = delivery.pieces;
		return result;
	}

	void fail(int status, const char* error) {
		REQUIRE(receiver_ != nullptr);
		if (status != 0) {
			hc_test::respond(*receiver_, status, "<html>error</html>");
		} else {
			hc_test::respond_failure(*receiver_, error);
		}
		receiver_ = nullptr;
	}

	/// Hands the receiver to the test, which drives the response itself (the fetcher is idle then).
	hc::FetchReceiver& take() {
		REQUIRE(receiver_ != nullptr);
		hc::FetchReceiver* receiver = receiver_;
		receiver_ = nullptr;
		return *receiver;
	}

	std::vector<std::string> urls;
	size_t pieces = 0;

private:

	hc::FetchReceiver* receiver_ = nullptr;
};

std::string sha256_hex(const std::string& data) {
	uint8_t digest[hc::Sha256::kDigestSize];
	hc::Sha256::hash(data.data(), data.size(), digest);
	char hex[65];
	hc::to_hex(digest, sizeof(digest), hex);
	return hex;
}

/// A stand-in for a gzip-compressed pack: the device never looks inside.
const std::string kGermanPack = std::string("\x1f\x8b\x08\x00", 4) + "{\"nav.overview\":\"\xc3\x9c" "bersicht\"}";

std::string entry(const std::string& code, const std::string& name, const std::string& path, const std::string& body) {
	return "\"" + code + "\": {\"name\": \"" + name + "\", \"path\": \"" + path + "\", \"sha256\": \"" + sha256_hex(body) +
		"\", \"size\": " + std::to_string(body.size()) + "}";
}

std::string manifest(const std::string& entries, const std::string& version = "0.1.0-dev") {
	return "{\"version\": \"" + version + "\", \"packs\": {" + entries + "}}";
}

const std::string kManifestUrl = "https://updates.example/hc/0.1.0-dev/i18n/manifest.json";
const std::string kPackUrl = "https://updates.example/hc/0.1.0-dev/i18n/lang-de.json.gz";

struct Fixture {
	ScopedCapture capture{hc::LogLevel::debug};
	FakeClock clock;
	hc::EventLoop loop;
	FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	FakeTimeZone zone;
	FakeSettings settings;
	hc::TimeService time{loop, clock, zone, settings};
	ScriptedFetcher fetcher;
	FakeBlobStore store;
	// The build metadata does not name a release directory.
	LanguagePacks packs{loop, time, settings, connectivity, fetcher, store, "0.1.0-dev+g1a2b3c4"};

	explicit Fixture(bool ready = true) {
		connectivity.start();
		if (ready) {
			clock.wall_seconds = 1792270800; // 2026-10-17
			clock.synchronized = true;
			connectivity.link_up("192.0.2.100");
			settings.set_string(hc::setting::kUpdateUrl, "https://updates.example/hc/");
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

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	/// Boots and runs until the first fetch is asked for.
	void boot() {
		packs.start();
		run_for(LanguagePacks::kStartDelayMs + 1);
	}

	std::string blob(const char* name) const {
		auto it = store.blobs.find(name);
		return it == store.blobs.end() ? std::string("<none>") : std::string(it->second.begin(), it->second.end());
	}
};

} // namespace

// ---- The setting ----------------------------------------------------------------------------

TEST_CASE("Lang: ui.lang accepts two-letter tags with an optional region and refuses the rest") {
	for (const char* good : {"en", "de", "pt-BR", "zh-CN"}) {
		CHECK_MESSAGE(hc::ui_lang_valid(good), good);
	}
	for (const char* bad : {"", "e", "EN", "De", "deu", "de-de", "de_DE", "de-DEX", "de-D1", "d1", "../x", "de/", "en "}) {
		const char* error = nullptr;
		CHECK_MESSAGE(!hc::ui_lang_valid(bad, &error), bad);
		CHECK(error != nullptr);
	}
	CHECK(!hc::ui_lang_valid(nullptr));
}

TEST_CASE("Lang: ui.lang reads its default, a stored tag, and falls back (logged) from an invalid one") {
	ScopedCapture capture;
	FakeSettings settings;
	char lang[hc::setting::kMaxUiLangLength + 1];
	hc::read_ui_lang(settings, lang, "test");
	CHECK(std::string(lang) == "en");
	settings.set_string(hc::setting::kUiLang, "pt-BR");
	hc::read_ui_lang(settings, lang, "test");
	CHECK(std::string(lang) == "pt-BR");
	// Longer than a tag: not truncated into a valid "en-GB".
	settings.set_string(hc::setting::kUiLang, "en-GBX");
	hc::read_ui_lang(settings, lang, "test");
	CHECK(std::string(lang) == "en");
	CHECK(capture.sink.contains("setting ui.lang is invalid"));

	const char* error = nullptr;
	CHECK(!hc::store_ui_lang(settings, "../../etc", &error));
	CHECK(std::string(error).find("two lower-case letters") != std::string::npos);
	CHECK(settings.entries["ui.lang"].text == "en-GBX"); // unchanged
	CHECK(hc::store_ui_lang(settings, "de", &error));
	CHECK(settings.entries["ui.lang"].text == "de");
}

TEST_CASE("Lang: the web API lists ui.lang, validates a change and tells the session the language") {
	hc_test::WebFixture f;
	std::string cookie = f.onboard();
	auto session = f.get("/api/auth/session");
	CHECK(session.json["lang"].text == "en");

	auto list = f.get("/api/settings", cookie);
	bool found = false;
	for (const auto& s : list.json["settings"].items) {
		if (s["key"].text == "ui.lang") {
			found = true;
			CHECK(s["value"].text == "en");
			CHECK(s["default"].text == "en");
			CHECK(s["editable"].boolean);
		}
	}
	CHECK(found);
	CHECK(!list.json.has("i18n")); // no language packs attached

	auto bad = f.request(hc::HttpMethod::put, "/api/settings/ui.lang", "{\"value\":\"de-de\"}", cookie);
	CHECK(bad.status == 422);
	CHECK(bad.json["code"].text == "settings.invalid");
	CHECK(bad.json["error"].text.find("two lower-case letters") != std::string::npos);
	CHECK(f.fake.settings.entries.count("ui.lang") == 0);

	// With the language packs attached, a change starts their sync and the list reports them.
	ScriptedFetcher fetcher;
	FakeBlobStore store;
	LanguagePacks packs{f.controller.loop(), f.controller.time(), f.fake.settings, f.controller.connectivity(), fetcher,
		store, "0.1.0-dev"};
	f.api.set_language_packs(&packs);
	auto good = f.request(hc::HttpMethod::put, "/api/settings/ui.lang", "{\"value\":\"de\"}", cookie);
	REQUIRE(good.status == 200);
	CHECK(f.fake.settings.entries["ui.lang"].text == "de");
	CHECK(packs.lang() == std::string("de"));
	CHECK(packs.pending());
	CHECK(good.json["i18n"]["lang"].text == "de");
	CHECK(good.json["i18n"]["state"].text == "waiting");
	CHECK(good.json["i18n"]["release"].text == "0.1.0-dev");
	CHECK(f.get("/api/auth/session").json["lang"].text == "de");
}

// ---- The sync decision ----------------------------------------------------------------------

TEST_CASE("Lang: blob names and manifest URLs") {
	char name[32];
	CHECK(LanguagePacks::blob_name("pt-BR", name, sizeof(name)));
	CHECK(std::string(name) == "i18n.pt-BR.json.gz");
	CHECK(hc::BlobStore::check_name(name) == hc::BlobStatus::ok);
	CHECK(!LanguagePacks::blob_name("../x", name, sizeof(name)));
	char url[128];
	CHECK(LanguagePacks::manifest_url("http://desk:8070//", "0.2.0+g1", url, sizeof(url)));
	CHECK(std::string(url) == "http://desk:8070/0.2.0/i18n/manifest.json");
	CHECK(!LanguagePacks::manifest_url("http://desk:8070", "0.2.0", url, 20));
}

TEST_CASE("Lang: nothing is fetched before the network, the clock and an update source are there") {
	Fixture f(false);
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.settings.set_string(hc::setting::kUpdateChannel, hc::setting::kUpdateChannelOff);
	f.boot();
	CHECK(f.fetcher.urls.empty());
	CHECK(f.packs.state() == LanguagePackState::waiting);
	CHECK(std::string(f.packs.detail()) == "the network is down");

	f.connectivity.link_up("192.0.2.100");
	f.run_for(LanguagePacks::kReadyPollMs);
	CHECK(f.fetcher.urls.empty());
	CHECK(std::string(f.packs.detail()).find("clock is not synchronized") != std::string::npos);

	f.clock.wall_seconds = 1792270800;
	f.clock.synchronized = true;
	f.run_for(LanguagePacks::kReadyPollMs);
	CHECK(f.fetcher.urls.empty());
	CHECK(std::string(f.packs.detail()).find("update channel is off") != std::string::npos);
	// The reason is logged when it changes, not at every poll.
	f.run_for(3 * LanguagePacks::kReadyPollMs);
	CHECK(f.capture.sink.count("update channel is off") == 1);

	f.settings.set_string(hc::setting::kUpdateUrl, "https://updates.example/hc");
	f.run_for(LanguagePacks::kReadyPollMs);
	REQUIRE(f.fetcher.urls.size() == 1);
	CHECK(f.fetcher.urls[0] == kManifestUrl);
	CHECK(f.packs.state() == LanguagePackState::fetching);
}

TEST_CASE("Lang: a German device downloads its pack once, verifies it and keeps it across a restart") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.fr.json.gz"] = {'o', 'l', 'd'}; // a previous language's pack
	f.store.blobs["h.kitchen"] = {'x'};                 // somebody else's blob
	f.boot();
	REQUIRE(f.fetcher.urls.size() == 1);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack) + ", " +
		entry("pt-BR", "Português (Brasil)", "lang-pt-BR.json.gz", "x")));
	CHECK(f.packs.offer_count() == 2);
	CHECK(f.packs.offer(1).code == std::string("pt-BR"));
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 2);
	CHECK(f.fetcher.urls[1] == kPackUrl);
	f.fetcher.ok(kGermanPack);
	CHECK(f.packs.state() == LanguagePackState::installed);
	CHECK(f.blob("i18n.de.json.gz") == kGermanPack);
	CHECK(f.store.blobs.count("i18n.fr.json.gz") == 0);
	CHECK(f.store.blobs.count("h.kitchen") == 1);
	CHECK(!f.packs.pending());
	f.run_for(24 * 3600 * 1000ull);
	CHECK(f.fetcher.urls.size() == 2); // no periodic re-sync

	// The next boot asks the manifest again (an OTA update brings another release) but finds the
	// stored pack up to date.
	LanguagePacks again{f.loop, f.time, f.settings, f.connectivity, f.fetcher, f.store, "0.1.0-dev"};
	again.start();
	f.run_for(LanguagePacks::kStartDelayMs + 1);
	REQUIRE(f.fetcher.urls.size() == 3);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)));
	f.run_for(1000);
	CHECK(f.fetcher.urls.size() == 3);
	CHECK(again.state() == LanguagePackState::installed);
	CHECK(std::string(again.detail()).find("up to date") != std::string::npos);

	char buffer[1024];
	hc::JsonWriter json(buffer, sizeof(buffer));
	json.begin_object();
	again.write_json(json);
	json.end_object();
	Json parsed;
	std::string error;
	REQUIRE(JsonReader(std::string(json.data(), json.length())).parse(parsed, error));
	CHECK(parsed["state"].text == "installed");
	CHECK(parsed["offered"].items[0]["name"].text == "Deutsch");
}

TEST_CASE("Lang: a pack whose SHA-256 or size differs from the manifest is refused and the old pack kept") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	f.boot();
	std::string published = manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack));
	f.fetcher.ok(published);
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 2);
	std::string tampered = kGermanPack;
	tampered.back() = '!';
	f.fetcher.ok(tampered);
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()).find("does not match the manifest") != std::string::npos);
	CHECK(f.capture.sink.contains("refused"));
	CHECK(f.blob("i18n.de.json.gz") == "old");

	// Retried after the backoff, the whole pass again.
	f.run_for(LanguagePacks::kRetryInitialMs - 10);
	CHECK(f.fetcher.urls.size() == 2);
	f.run_for(20);
	REQUIRE(f.fetcher.urls.size() == 3);
	f.fetcher.ok(published);
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 4);
	f.fetcher.ok(kGermanPack + "trailing");
	CHECK(std::string(f.packs.detail()).find("the manifest says") != std::string::npos);
	CHECK(f.blob("i18n.de.json.gz") == "old");
	// The second failure waits twice as long.
	f.run_for(2 * LanguagePacks::kRetryInitialMs - 10);
	CHECK(f.fetcher.urls.size() == 4);
	f.run_for(20);
	REQUIRE(f.fetcher.urls.size() == 5);
	f.fetcher.ok(published);
	f.run_for(1);
	f.fetcher.ok(kGermanPack);
	CHECK(f.packs.state() == LanguagePackState::installed);
	CHECK(f.blob("i18n.de.json.gz") == kGermanPack);
}

TEST_CASE("Lang: a language the release does not offer gets no pack, and malformed entries are refused") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "fr");
	f.store.blobs["i18n.fr.json.gz"] = {'o', 'l', 'd'}; // of an older release: must not shadow the bundled UI
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack) + ", " +
		entry("FR", "Français", "lang-fr.json.gz", "x") + ", " + entry("es", "Español", "../lang-es.json.gz", "x") + ", " +
		entry("../x", "Evil", "evil.json.gz", "x")));
	f.run_for(1000);
	CHECK(f.fetcher.urls.size() == 1);
	CHECK(f.packs.state() == LanguagePackState::not_offered);
	CHECK(std::string(f.packs.detail()) == "release 0.1.0-dev offers no pack for 'fr'");
	CHECK(f.store.blobs.count("i18n.fr.json.gz") == 0);
	CHECK(f.capture.sink.contains("manifest entry 'FR' refused: not a language tag"));
	CHECK(f.capture.sink.contains("manifest entry 'es' refused: \"path\" must be a plain file name"));
	CHECK(f.capture.sink.contains("manifest entry '../x' refused"));
	REQUIRE(f.packs.offer_count() == 1);
	CHECK(f.packs.offer(0).code == std::string("de"));
	CHECK(!f.packs.pending());
}

TEST_CASE("Lang: the wanted language's malformed entry and a manifest of another release are failures") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "sub/lang-de.json.gz", kGermanPack)));
	f.run_for(1);
	CHECK(f.fetcher.urls.size() == 1);
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()).find("plain file name") != std::string::npos);

	f.run_for(LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 2);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack), "0.2.0"));
	CHECK(std::string(f.packs.detail()).find("another release") != std::string::npos);

	f.run_for(2 * LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 3);
	std::string bad = manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack));
	bad.replace(bad.find("\"size\": ") + 8, std::to_string(kGermanPack.size()).size(), "12.5");
	f.fetcher.ok(bad);
	CHECK(std::string(f.packs.detail()).find("\"size\" must be a whole number of bytes") != std::string::npos);

	f.run_for(4 * LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 4);
	f.fetcher.ok("not json");
	CHECK(f.packs.state() == LanguagePackState::failed);
	f.run_for(8 * LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 5);
	f.fetcher.fail(404, "HTTP status 404");
	CHECK(std::string(f.packs.detail()).find("HTTP status 404") != std::string::npos);
	CHECK(f.store.blobs.empty());
}

TEST_CASE("Lang: English needs no pack, removes stored packs and still records the offers") {
	Fixture f;
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	f.boot();
	CHECK(f.store.blobs.empty()); // removed without waiting for the network
	REQUIRE(f.fetcher.urls.size() == 1);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)));
	f.run_for(1000);
	CHECK(f.fetcher.urls.size() == 1);
	CHECK(f.packs.state() == LanguagePackState::builtin);
	CHECK(f.packs.offer_count() == 1);

	// Without an update source English is still "builtin", with the reason as the detail.
	Fixture g(false);
	g.boot();
	CHECK(g.packs.state() == LanguagePackState::builtin);
	CHECK(std::string(g.packs.detail()) == "the network is down");
}

TEST_CASE("Lang: a language change during a fetch discards its result and syncs the new language") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "pt-BR");
	f.boot();
	REQUIRE(f.fetcher.urls.size() == 1);
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.packs.language_changed();
	const char* error = nullptr;
	CHECK(!f.packs.sync_now(&error));
	CHECK(std::string(error) == "a sync is running");
	f.fetcher.ok(manifest(entry("pt-BR", "Português", "lang-pt-BR.json.gz", "pt")));
	CHECK(f.packs.offer_count() == 0); // discarded
	f.run_for(LanguagePacks::kChangeDelayMs + 1);
	REQUIRE(f.fetcher.urls.size() == 2);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)));
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 3);
	f.fetcher.ok(kGermanPack);
	CHECK(f.packs.state() == LanguagePackState::installed);
	CHECK(f.store.blobs.count("i18n.pt-BR.json.gz") == 0);
}

TEST_CASE("Lang: a lost fetch result counts as a failure after the guard time") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.boot();
	REQUIRE(f.fetcher.pending());
	f.run_for(LanguagePacks::kFetchTimeoutMs + 1);
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()).find("no answer") != std::string::npos);
	// The late result is ignored; the retry follows the backoff.
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)));
	CHECK(f.packs.state() == LanguagePackState::failed);
	f.run_for(LanguagePacks::kRetryInitialMs + 1);
	CHECK(f.fetcher.urls.size() == 2);
}

TEST_CASE("Lang: a store that cannot write fails the pass with its reason") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.fail_writes = true;
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)));
	f.run_for(1);
	f.fetcher.ok(kGermanPack);
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()).find("cannot store the pack i18n.de.json.gz: storage error") != std::string::npos);
}

// ---- Streaming (issue #43) -------------------------------------------------------------------

namespace {

/// A pack of `size` bytes that does not compress (the device never looks inside).
std::string big_pack(size_t size) {
	std::string pack(size, '\0');
	uint32_t x = 2463534242u;
	for (char& c : pack) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		c = static_cast<char>(x);
	}
	return pack;
}

/// Lists the blobs the store shows (what the web server and every reader see).
struct Listing : hc::BlobVisitor {
	std::map<std::string, size_t> names;
	void blob(const char* name, size_t size) override { names[name] = size; }
};

} // namespace

TEST_CASE("Lang: a 40 KB pack streams in 1 KB pieces into the store, hashed on the way") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	std::string pack = big_pack(40 * 1024);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 2);
	hc::FetchResult result = f.fetcher.ok(pack, 1024);
	CHECK(result.ok);
	CHECK(f.fetcher.pieces == 40);
	CHECK(f.store.opens == 1);
	CHECK(f.store.appends == 40);
	CHECK(f.store.appended_bytes == pack.size());
	CHECK(f.store.commits == 1);
	CHECK(f.store.temporaries.empty());
	CHECK(f.packs.state() == LanguagePackState::installed);
	CHECK(f.blob("i18n.de.json.gz") == pack);
	CHECK(std::string(f.packs.detail()) == "pack 'de' of release 0.1.0-dev installed (40960 bytes)");

	// The stored pack's hash is the manifest's: the next pass fetches nothing.
	REQUIRE(f.packs.sync_now(nullptr));
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 3);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1000);
	CHECK(f.fetcher.urls.size() == 3);
	CHECK(std::string(f.packs.detail()).find("up to date") != std::string::npos);
}

TEST_CASE("Lang: while the pack streams, only its temporary blob grows; readers see the old pack") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	std::string pack = big_pack(5000);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	hc::FetchReceiver& receiver = f.fetcher.take();
	REQUIRE(receiver.fetch_begin(200, pack.size()));
	REQUIRE(receiver.fetch_chunk(pack.data(), 3000));
	// Mid-stream: the pack's name still lists the old content; the temporary name is not listed.
	Listing listing;
	f.store.visit(listing);
	CHECK(listing.names.size() == 1);
	CHECK(listing.names["i18n.de.json.gz"] == 3);
	CHECK(f.blob("i18n.de.json.gz") == "old");
	REQUIRE(f.store.temporaries.count(".i18n.de.json.gz.tmp") == 1);
	CHECK(f.store.temporaries[".i18n.de.json.gz.tmp"].size() == 3000);
	REQUIRE(receiver.fetch_chunk(pack.data() + 3000, pack.size() - 3000));
	CHECK(f.blob("i18n.de.json.gz") == "old"); // not before the end and the hash check
	hc::FetchResult end;
	end.ok = end.complete = true;
	end.http_status = 200;
	end.length = pack.size();
	receiver.fetch_end(end);
	CHECK(f.blob("i18n.de.json.gz") == pack);
	CHECK(f.store.temporaries.empty());
}

TEST_CASE("Lang: a hash mismatch discards the temporary blob and keeps the previous pack") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	std::string pack = big_pack(40 * 1024);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	std::string tampered = pack;
	tampered[20000] ^= 0x01;
	CHECK(f.fetcher.ok(tampered, 1024).ok);
	CHECK(f.store.appended_bytes == pack.size());
	CHECK(f.store.discards == 1);
	CHECK(f.store.commits == 0);
	CHECK(f.store.temporaries.empty());
	CHECK(f.blob("i18n.de.json.gz") == "old");
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()).find("does not match the manifest") != std::string::npos);
}

TEST_CASE("Lang: a pack larger than the free space is refused before a byte is written") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.free_bytes = 10000;
	std::string pack = big_pack(40 * 1024);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	CHECK(f.fetcher.urls.size() == 1); // not even fetched
	CHECK(f.store.opens == 0);
	CHECK(f.store.appended_bytes == 0);
	CHECK(f.packs.state() == LanguagePackState::failed);
	CHECK(std::string(f.packs.detail()) ==
		"pack " + kPackUrl + " refused: 40960 bytes, the blob store has 10000 bytes free");
	CHECK(f.capture.sink.contains("the blob store has 10000 bytes free"));
}

TEST_CASE("Lang: a body longer than the manifest's size is refused where it crosses it") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	std::string pack = big_pack(4000);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	// Chunked (no Content-Length): the receiver counts.
	hc::FetchResult result = f.fetcher.ok(pack + big_pack(3000), 1024, false);
	CHECK(result.aborted);
	CHECK(f.store.appended_bytes <= pack.size());
	CHECK(f.store.discards == 1);
	CHECK(f.blob("i18n.de.json.gz") == "old");
	CHECK(std::string(f.packs.detail()) == "pack " + kPackUrl + " refused: longer than the 4000 bytes the manifest says");

	// Announced: refused at the begin, nothing opened.
	f.run_for(LanguagePacks::kRetryInitialMs);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	int opens = f.store.opens;
	result = f.fetcher.ok(pack + "x");
	CHECK(result.aborted);
	CHECK(result.length == 0);
	CHECK(f.store.opens == opens);
	CHECK(std::string(f.packs.detail()) ==
		"pack " + kPackUrl + " refused: the server announces 4001 bytes, the manifest says 4000");
}

TEST_CASE("Lang: a store that fills up during the stream fails with its reason and keeps the old pack") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.store.blobs["i18n.de.json.gz"] = {'o', 'l', 'd'};
	std::string pack = big_pack(8000);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	// The space shrinks between the check and the open (another writer): the store refuses the open.
	f.store.free_bytes = 5000;
	CHECK(f.fetcher.ok(pack, 1024).aborted);
	CHECK(f.store.opens == 0);
	CHECK(std::string(f.packs.detail()) == "cannot store the pack i18n.de.json.gz: no space");

	// It shrinks during the stream: the append fails, the temporary blob goes.
	f.store.free_bytes = SIZE_MAX;
	f.run_for(LanguagePacks::kRetryInitialMs);
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	hc::FetchReceiver& receiver = f.fetcher.take();
	REQUIRE(receiver.fetch_begin(200, pack.size()));
	REQUIRE(receiver.fetch_chunk(pack.data(), 4096));
	f.store.free_bytes = 5000;
	CHECK_FALSE(receiver.fetch_chunk(pack.data() + 4096, 1024));
	hc::FetchResult end;
	end.aborted = true;
	end.http_status = 200;
	end.error = "aborted by the receiver";
	receiver.fetch_end(end);
	CHECK(f.store.discards == 1);
	CHECK(f.store.temporaries.empty());
	CHECK(f.blob("i18n.de.json.gz") == "old");
	CHECK(std::string(f.packs.detail()) == "cannot store the pack " + kPackUrl + " after 4096 bytes: no space");
}

TEST_CASE("Lang: the manifest reader refuses an oversize manifest with its own limit") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	f.boot();
	std::string padded = manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack));
	padded.insert(1, "\"pad\": \"" + std::string(LanguagePacks::kMaxManifest, ' ') + "\", ");
	hc::FetchResult result = f.fetcher.ok(padded);
	CHECK(result.aborted);
	CHECK(std::string(f.packs.detail()) == "manifest " + kManifestUrl +
		" refused: the manifest is larger than 4096 bytes (" + std::to_string(padded.size()) + " announced)");
	f.run_for(LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 2);
	result = f.fetcher.ok(padded, 512, false);
	CHECK(result.aborted);
	CHECK(result.length == LanguagePacks::kMaxManifest);
	CHECK(std::string(f.packs.detail()) == "manifest " + kManifestUrl + " refused: the manifest is larger than 4096 bytes");
	// A manifest in many small pieces is put together.
	f.run_for(2 * LanguagePacks::kRetryInitialMs);
	REQUIRE(f.fetcher.urls.size() == 3);
	CHECK(f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", kGermanPack)), 5, false).ok);
	f.run_for(1);
	REQUIRE(f.fetcher.urls.size() == 4);
	f.fetcher.ok(kGermanPack, 3);
	CHECK(f.packs.state() == LanguagePackState::installed);
}

TEST_CASE("Lang: a language change during the pack's stream aborts it and discards the temporary blob") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	std::string pack = big_pack(5000);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	hc::FetchReceiver& receiver = f.fetcher.take();
	REQUIRE(receiver.fetch_begin(200, pack.size()));
	REQUIRE(receiver.fetch_chunk(pack.data(), 1000));
	f.settings.set_string(hc::setting::kUiLang, "fr");
	f.packs.language_changed();
	CHECK_FALSE(receiver.fetch_chunk(pack.data() + 1000, 1000));
	hc::FetchResult end;
	end.aborted = true;
	end.http_status = 200;
	end.error = "aborted by the receiver";
	receiver.fetch_end(end);
	CHECK(f.store.temporaries.empty());
	CHECK(f.store.discards == 1);
	CHECK(f.store.blobs.count("i18n.de.json.gz") == 0);
	f.run_for(LanguagePacks::kChangeDelayMs + 1);
	CHECK(f.fetcher.urls.size() == 3); // the new language's manifest
}

TEST_CASE("Lang: a lost pack fetch drops its temporary blob at the guard time") {
	Fixture f;
	f.settings.set_string(hc::setting::kUiLang, "de");
	std::string pack = big_pack(5000);
	f.boot();
	f.fetcher.ok(manifest(entry("de", "Deutsch", "lang-de.json.gz", pack)));
	f.run_for(1);
	hc::FetchReceiver& receiver = f.fetcher.take();
	REQUIRE(receiver.fetch_begin(200, pack.size()));
	REQUIRE(receiver.fetch_chunk(pack.data(), 1000));
	f.run_for(LanguagePacks::kFetchTimeoutMs);
	CHECK(f.store.temporaries.empty());
	CHECK(f.store.discards == 1);
	CHECK(std::string(f.packs.detail()).find("no answer to the fetch") != std::string::npos);
	// The late pieces and end are refused or ignored.
	CHECK_FALSE(receiver.fetch_chunk(pack.data() + 1000, 1000));
	hc::FetchResult end;
	end.aborted = true;
	receiver.fetch_end(end);
	CHECK(f.store.commits == 0);
}

// ---- The web UI's packs ---------------------------------------------------------------------

namespace {

std::string read_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	REQUIRE_MESSAGE(in.good(), path.string());
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

Json read_pack(const std::filesystem::path& path) {
	Json json;
	std::string error;
	REQUIRE_MESSAGE(JsonReader(read_file(path)).parse(json, error), path.string() << ": " << error);
	REQUIRE(json.kind == Json::Kind::object);
	return json;
}

std::set<std::string> placeholders(const std::string& text) {
	std::set<std::string> names;
	static const std::regex kPlaceholder("\\{(\\w+)\\}");
	for (auto it = std::sregex_iterator(text.begin(), text.end(), kPlaceholder); it != std::sregex_iterator(); ++it) {
		names.insert((*it)[1].str());
	}
	return names;
}

} // namespace

TEST_CASE("Lang: every committed pack has exactly the keys and placeholders of the English base") {
	const std::filesystem::path web(HC_WEB_DIR);
	Json base = read_pack(web / "ui" / "dist" / "lang" / "en.json");
	CHECK(base.members.size() > 250);
	CHECK(base["lang.name"].text == "English");
	std::vector<std::filesystem::path> packs;
	for (const auto& file : std::filesystem::directory_iterator(web / "ui" / "lang")) {
		if (file.path().extension() == ".json") {
			packs.push_back(file.path());
		}
	}
	REQUIRE(!packs.empty());
	std::string loader = read_file(web / "ui" / "dist" / "i18n.js");
	bool german = false;
	for (const auto& path : packs) {
		std::string code = path.stem().string();
		INFO("pack " << code);
		CHECK(hc::ui_lang_valid(code.c_str()));
		german = german || code == "de";
		// The UI offers every bundled pack in its language selector.
		CHECK(loader.find("\"" + code + "\":") != std::string::npos);
		Json pack = read_pack(path);
		for (const auto& [key, value] : base.members) {
			INFO("key " << key);
			if (!pack.has(key)) {
				FAIL_CHECK("missing in " << code << ".json: " << key);
				continue;
			}
			CHECK(pack[key].kind == Json::Kind::string);
			CHECK(!pack[key].text.empty());
			CHECK(placeholders(pack[key].text) == placeholders(value.text));
		}
		for (const auto& [key, value] : pack.members) {
			CHECK_MESSAGE(base.has(key), "extra in " << code << ".json: " << key);
		}
	}
	CHECK(german);
}
