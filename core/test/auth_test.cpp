/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/auth.h"
#include "hc/network_settings.h"
#include "web_test_support.h"

using hc::Auth;
using hc::AuthStatus;
using hc::HashParams;
using hc::SessionRef;

namespace {

struct AuthFixture {
	hc_test::FakeClock clock;
	hc_test::FakeSettings settings;
	hc_test::FakeCrypto crypto;
	Auth auth{settings, clock, crypto};

	void derive(const std::string& password, const HashParams& params, uint8_t key[Auth::kKeySize]) {
		crypto.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(), params.salt,
			sizeof(params.salt), params.iterations, key, Auth::kKeySize);
	}

	std::string onboard(const std::string& password) {
		HashParams params;
		REQUIRE(auth.begin_onboarding(params) == AuthStatus::ok);
		uint8_t key[Auth::kKeySize];
		derive(password, params, key);
		char session[Auth::kSessionText];
		REQUIRE(auth.finish_onboarding(params, key, "192.0.2.1", session) == AuthStatus::ok);
		return session;
	}

	AuthStatus login(const std::string& password, const char* source, std::string* session = nullptr,
		uint32_t* retry = nullptr) {
		HashParams params;
		uint32_t retry_after = 0;
		AuthStatus status = auth.begin_login(source, params, retry_after);
		if (retry != nullptr) {
			*retry = retry_after;
		}
		if (status != AuthStatus::ok) {
			return status;
		}
		uint8_t key[Auth::kKeySize];
		derive(password, params, key);
		char text[Auth::kSessionText];
		uint32_t failures = 0;
		status = auth.finish_login(source, params, key, text, retry_after, failures);
		if (retry != nullptr) {
			*retry = retry_after;
		}
		if (status == AuthStatus::ok && session != nullptr) {
			*session = text;
		}
		return status;
	}
};

} // namespace

TEST_CASE("AuthPassword record formats and parses as the persisted format") {
	HashParams params;
	for (size_t i = 0; i < sizeof(params.salt); ++i) {
		params.salt[i] = static_cast<uint8_t>(i);
	}
	params.iterations = 10000;
	uint8_t key[Auth::kKeySize];
	for (size_t i = 0; i < sizeof(key); ++i) {
		key[i] = static_cast<uint8_t>(0xA0 + i);
	}
	char text[hc::Settings::kMaxStringLength + 1];
	REQUIRE(Auth::format_record(params, key, text, sizeof(text)));
	// The frozen format: scheme, iterations, 32 hex digits of salt, 64 of key.
	CHECK(std::string(text) ==
		"pbkdf2-sha256$10000$000102030405060708090a0b0c0d0e0f$"
		"a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
	CHECK(std::strlen(text) <= hc::Settings::kMaxStringLength);
	HashParams back;
	uint8_t key_back[Auth::kKeySize];
	REQUIRE(Auth::parse_record(text, back, key_back));
	CHECK(back.iterations == 10000);
	CHECK(std::memcmp(back.salt, params.salt, sizeof(back.salt)) == 0);
	CHECK(std::memcmp(key_back, key, sizeof(key)) == 0);
	CHECK_FALSE(Auth::parse_record("pbkdf2-sha1$1$00$00", back, key_back));
	CHECK_FALSE(Auth::parse_record("pbkdf2-sha256$0$000102030405060708090a0b0c0d0e0f$00", back, key_back));
	CHECK_FALSE(Auth::parse_record("pbkdf2-sha256$10$0001$a0", back, key_back));
}

TEST_CASE("AuthOnboarding happens once, survives a restart and then needs a login") {
	AuthFixture f;
	f.auth.start();
	CHECK_FALSE(f.auth.has_password());
	HashParams params;
	uint32_t retry = 0;
	CHECK(f.auth.begin_login("192.0.2.1", params, retry) == AuthStatus::no_password);
	std::string session = f.onboard("correct horse");
	CHECK(f.auth.has_password());
	CHECK(session.size() == 32);
	CHECK(f.auth.begin_onboarding(params) == AuthStatus::password_set);
	// The stored record is the hash, never the password.
	std::string stored = f.settings.entries[Auth::kPasswordKey].text;
	CHECK(stored.find("pbkdf2-sha256$10000$") == 0);
	CHECK(stored.find("correct horse") == std::string::npos);
	CHECK(hc::setting_is_secret(Auth::kPasswordKey));
	CHECK(hc::setting_is_secret("web.tok.3"));

	// A restart: sessions are gone, the password stays.
	Auth restarted(f.settings, f.clock, f.crypto);
	restarted.start();
	CHECK(restarted.has_password());
	CHECK_FALSE(restarted.session_from_cookie(("hc_session=" + session).c_str()).valid());
	CHECK(f.auth.session_from_cookie(("other=1; hc_session=" + session + "; x=y").c_str()).valid());
}

TEST_CASE("AuthPassword rules refuse short, long and control-character passwords") {
	const char* error = nullptr;
	CHECK_FALSE(Auth::password_valid("short", 5, &error));
	CHECK(std::string(error).find("8 to 64") != std::string::npos);
	CHECK(Auth::password_valid("12345678", 8, &error));
	std::string long_one(65, 'x');
	CHECK_FALSE(Auth::password_valid(long_one.c_str(), long_one.size(), &error));
	CHECK_FALSE(Auth::password_valid("1234\n5678", 9, &error));
	CHECK(Auth::password_valid("Grüße 1234", std::strlen("Grüße 1234"), &error));
}

TEST_CASE("AuthLogin rate limits a source after five failures with a doubling block") {
	AuthFixture f;
	f.auth.start();
	f.onboard("correct horse");
	uint32_t retry = 0;
	for (int i = 1; i <= 4; ++i) {
		CHECK(f.login("wrong", "192.0.2.9", nullptr, &retry) == AuthStatus::wrong_password);
		CHECK(retry == 0);
	}
	CHECK(f.login("wrong", "192.0.2.9", nullptr, &retry) == AuthStatus::wrong_password);
	CHECK(retry == 30);
	// Blocked, even with the right password; other sources are not affected.
	CHECK(f.login("correct horse", "192.0.2.9", nullptr, &retry) == AuthStatus::rate_limited);
	CHECK(retry == 30);
	CHECK(f.login("correct horse", "192.0.2.10") == AuthStatus::ok);
	f.clock.advance(30000);
	CHECK(f.login("wrong", "192.0.2.9", nullptr, &retry) == AuthStatus::wrong_password);
	CHECK(retry == 60);
	f.clock.advance(60000);
	// A success clears the source.
	std::string session;
	CHECK(f.login("correct horse", "192.0.2.9", &session) == AuthStatus::ok);
	for (int i = 1; i <= 4; ++i) {
		CHECK(f.login("wrong", "192.0.2.9", nullptr, &retry) == AuthStatus::wrong_password);
		CHECK(retry == 0);
	}
	// The block is capped at 15 minutes.
	for (int i = 0; i < 12; ++i) {
		f.login("wrong", "192.0.2.9", nullptr, &retry);
		f.clock.advance(Auth::kBlockMaxMs);
	}
	CHECK(f.login("wrong", "192.0.2.9", nullptr, &retry) == AuthStatus::wrong_password);
	CHECK(retry == Auth::kBlockMaxMs / 1000);
}

TEST_CASE("AuthLogin refuses a hash computed for an old password") {
	AuthFixture f;
	f.auth.start();
	f.onboard("correct horse");
	HashParams params;
	uint32_t retry = 0;
	REQUIRE(f.auth.begin_login("192.0.2.1", params, retry) == AuthStatus::ok);
	params.salt[0] ^= 1;
	uint8_t key[Auth::kKeySize] = {};
	char session[Auth::kSessionText];
	uint32_t failures = 0;
	CHECK(f.auth.finish_login("192.0.2.1", params, key, session, retry, failures) == AuthStatus::password_changed);
}

TEST_CASE("AuthSessions expire after 24 h idle and the least recently used one is replaced") {
	AuthFixture f;
	f.auth.start();
	std::string first = f.onboard("correct horse");
	SessionRef ref = f.auth.session_from_cookie(("hc_session=" + first).c_str());
	REQUIRE(ref.valid());
	f.clock.advance(Auth::kSessionIdleMs - 1);
	CHECK(f.auth.session_alive(ref));
	f.auth.touch(ref);
	f.clock.advance(Auth::kSessionIdleMs - 1);
	CHECK(f.auth.session_alive(ref));
	f.clock.advance(1);
	CHECK_FALSE(f.auth.session_alive(ref));
	CHECK_FALSE(f.auth.session_from_cookie(("hc_session=" + first).c_str()).valid());

	std::string sessions[Auth::kSessionSlots + 1];
	for (size_t i = 0; i <= Auth::kSessionSlots; ++i) {
		REQUIRE(f.login("correct horse", "192.0.2.1", &sessions[i]) == AuthStatus::ok);
		f.clock.advance(10);
	}
	CHECK(f.auth.session_count() == Auth::kSessionSlots);
	CHECK_FALSE(f.auth.session_from_cookie(("hc_session=" + sessions[0]).c_str()).valid());
	CHECK(f.auth.session_from_cookie(("hc_session=" + sessions[Auth::kSessionSlots]).c_str()).valid());

	// Logout ends it; the reference stays dead even when the slot is reused.
	SessionRef last = f.auth.session_from_cookie(("hc_session=" + sessions[Auth::kSessionSlots]).c_str());
	f.auth.end_session(last);
	CHECK_FALSE(f.auth.session_alive(last));
	CHECK_FALSE(f.auth.session_from_cookie("hc_session=zz").valid());
	CHECK_FALSE(f.auth.session_from_cookie(nullptr).valid());
}

TEST_CASE("AuthPassword clear ends all sessions and reopens onboarding") {
	AuthFixture f;
	f.auth.start();
	std::string session = f.onboard("correct horse");
	SessionRef ref = f.auth.session_from_cookie(("hc_session=" + session).c_str());
	CHECK(f.auth.clear_password() == AuthStatus::ok);
	CHECK_FALSE(f.auth.has_password());
	CHECK_FALSE(f.auth.session_alive(ref));
	CHECK(f.settings.entries.count(Auth::kPasswordKey) == 0);
	f.onboard("another one");
	CHECK(f.login("another one", "192.0.2.1") == AuthStatus::ok);
}

TEST_CASE("AuthTokens are shown once, stored hashed, survive a restart and can be revoked") {
	AuthFixture f;
	f.auth.start();
	char token[Auth::kTokenText];
	REQUIRE(f.auth.create_token("ehome", token) == AuthStatus::ok);
	CHECK(std::strlen(token) == 36);
	CHECK(std::strncmp(token, "hct_", 4) == 0);
	std::string stored = f.settings.entries["web.tok.0"].text;
	CHECK(stored.find("ehome$") == 0);
	CHECK(stored.find(token + 4) == std::string::npos);
	char refused[Auth::kTokenText];
	CHECK(f.auth.create_token("ehome", refused) == AuthStatus::name_taken);
	CHECK(refused[0] == '\0');
	CHECK(f.auth.create_token("bad name", refused) == AuthStatus::name_invalid);
	CHECK(f.auth.create_token("", refused) == AuthStatus::name_invalid);
	char second[Auth::kTokenText];
	REQUIRE(f.auth.create_token("script", second) == AuthStatus::ok);

	Auth restarted(f.settings, f.clock, f.crypto);
	restarted.start();
	CHECK(restarted.token_count() == 2);
	int slot = restarted.token_from_authorization((std::string("Bearer ") + token).c_str());
	REQUIRE(slot == 0);
	CHECK(std::string(restarted.token_name(0)) == "ehome");
	CHECK(restarted.token_from_authorization("Bearer hct_00000000000000000000000000000000") < 0);
	CHECK(restarted.token_from_authorization(token) < 0); // no scheme
	uint8_t hash[32];
	std::memcpy(hash, restarted.token_hash(slot), sizeof(hash));
	CHECK(restarted.token_alive(slot, hash));
	CHECK(restarted.revoke_token("ehome") == AuthStatus::ok);
	CHECK_FALSE(restarted.token_alive(slot, hash));
	CHECK(restarted.token_from_authorization((std::string("Bearer ") + token).c_str()) < 0);
	CHECK(restarted.revoke_token("ehome") == AuthStatus::not_found);
	CHECK(f.settings.entries.count("web.tok.0") == 0);

	for (int i = 0; i < 7; ++i) {
		REQUIRE(restarted.create_token(("t" + std::to_string(i)).c_str(), token) == AuthStatus::ok);
	}
	CHECK(restarted.create_token("one-too-many", token) == AuthStatus::capacity);
}

TEST_CASE("AuthRefuses without a random source and treats an unreadable record as absent") {
	AuthFixture f;
	f.settings.set_string(Auth::kPasswordKey, "garbage");
	f.settings.set_string("web.tok.1", "no-dollar");
	hc_test::ScopedCapture capture;
	f.auth.start();
	CHECK_FALSE(f.auth.has_password());
	CHECK(f.auth.token_count() == 0);
	CHECK(capture.sink.contains("unreadable"));
	f.crypto.fail = true;
	HashParams params;
	CHECK(f.auth.begin_onboarding(params) == AuthStatus::no_random);
	char token[Auth::kTokenText];
	CHECK(f.auth.create_token("x", token) == AuthStatus::no_random);
}
