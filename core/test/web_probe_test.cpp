/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the web server base (issue #5): the WebSocket decoder
// under fragmentation and interleaved control frames, the RFC 6455 accept key, protocol errors,
// the authentication life cycle across onboarding, clearing and tokens, and the log ring's
// sequence accounting.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/auth.h"
#include "hc/log_ring.h"
#include "hc/websocket.h"
#include "web_test_support.h"

namespace ws = hc::websocket;
using Event = ws::FrameDecoder::Event;
using hc::Auth;
using hc::AuthStatus;
using hc::HashParams;

namespace {

std::vector<uint8_t> masked(ws::Opcode opcode, bool fin, const std::string& payload) {
	static const uint8_t kMask[4] = {0x12, 0x34, 0x56, 0x78};
	uint8_t header[ws::kMaxHeader];
	size_t n = ws::encode_header(header, opcode, fin, payload.size(), kMask);
	std::vector<uint8_t> frame(header, header + n);
	std::vector<uint8_t> body(payload.begin(), payload.end());
	ws::apply_mask(body.data(), body.size(), kMask);
	frame.insert(frame.end(), body.begin(), body.end());
	return frame;
}

struct Decoder {
	uint8_t buffer[600] = {};
	ws::FrameDecoder decoder{buffer, sizeof(buffer)};
	std::vector<std::string> messages;
	std::vector<ws::Opcode> controls;
	uint16_t error_code = 0;

	/// Feeds the bytes in chunks of `chunk` bytes, collecting every event.
	void feed(const std::vector<uint8_t>& bytes, size_t chunk) {
		size_t at = 0;
		while (at < bytes.size()) {
			size_t n = std::min(chunk, bytes.size() - at);
			size_t consumed_total = 0;
			while (consumed_total < n) {
				size_t consumed = 0;
				Event e = decoder.push(bytes.data() + at + consumed_total, n - consumed_total, consumed);
				consumed_total += consumed;
				if (e == Event::message) {
					messages.emplace_back(reinterpret_cast<const char*>(decoder.data()), decoder.length());
				} else if (e == Event::control) {
					controls.push_back(decoder.opcode());
				} else if (e == Event::error) {
					error_code = decoder.close_code();
					return;
				}
				if (consumed == 0 && e == Event::need_more) {
					break;
				}
			}
			at += n;
		}
	}
};

struct AuthRig {
	hc_test::FakeClock clock;
	hc_test::FakeSettings settings;
	hc_test::FakeCrypto crypto;
	Auth auth{settings, clock, crypto};
	void derive(const std::string& password, const HashParams& params, uint8_t key[Auth::kKeySize]) {
		crypto.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(), params.salt,
			sizeof(params.salt), params.iterations, key, Auth::kKeySize);
	}
	AuthStatus onboard(const std::string& password, std::string* session = nullptr) {
		HashParams params;
		AuthStatus s = auth.begin_onboarding(params);
		if (s != AuthStatus::ok) {
			return s;
		}
		uint8_t key[Auth::kKeySize];
		derive(password, params, key);
		char text[Auth::kSessionText];
		s = auth.finish_onboarding(params, key, "192.0.2.1", text);
		if (s == AuthStatus::ok && session) {
			*session = text;
		}
		return s;
	}
	AuthStatus login(const std::string& password, const char* source, std::string* session = nullptr) {
		HashParams params;
		uint32_t retry = 0;
		AuthStatus s = auth.begin_login(source, params, retry);
		if (s != AuthStatus::ok) {
			return s;
		}
		uint8_t key[Auth::kKeySize];
		derive(password, params, key);
		char text[Auth::kSessionText];
		uint32_t failures = 0;
		s = auth.finish_login(source, params, key, text, retry, failures);
		if (s == AuthStatus::ok && session) {
			*session = text;
		}
		return s;
	}
};

} // namespace

TEST_CASE("WebProbe.FragmentedMaskedMessageWithAnInterleavedPingSurvivesAnyChunking") {
	std::string a(200, 'a'), b = "\xC3\xA4\xE2\x82\xAC", c(150, 'c'); // 'ä' and '€' are valid UTF-8
	std::vector<uint8_t> stream;
	auto append = [&](const std::vector<uint8_t>& f) { stream.insert(stream.end(), f.begin(), f.end()); };
	append(masked(ws::Opcode::text, false, a));
	append(masked(ws::Opcode::ping, true, "hi"));          // a control frame inside the fragmented message
	append(masked(ws::Opcode::continuation, false, b));
	append(masked(ws::Opcode::continuation, true, c));
	append(masked(ws::Opcode::text, true, "next"));
	for (size_t chunk : {1u, 3u, 7u, 64u, 1000u}) {
		Decoder d;
		d.feed(stream, chunk);
		CHECK(d.error_code == 0);
		REQUIRE(d.messages.size() == 2);
		CHECK(d.messages[0] == a + b + c);
		CHECK(d.messages[1] == "next");
		REQUIRE(d.controls.size() == 1);
		CHECK(d.controls[0] == ws::Opcode::ping);
	}
}

TEST_CASE("WebProbe.AcceptKeyMatchesTheRfcExample") {
	char out[ws::kAcceptKeyLength + 1];
	REQUIRE(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==", out));
	CHECK(std::string(out) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK_FALSE(ws::accept_key("", out));
}

TEST_CASE("WebProbe.ProtocolErrorsCloseWithTheRightCodes") {
	{
		Decoder d; // invalid UTF-8 in a text message
		d.feed(masked(ws::Opcode::text, true, std::string("ok\xFF\xFE")), 1000);
		CHECK(d.error_code == 1007);
	}
	{
		Decoder d; // a message larger than the buffer
		d.feed(masked(ws::Opcode::binary, true, std::string(601, 'x')), 1000);
		CHECK(d.error_code == 1009);
	}
	{
		Decoder d; // a control frame with a payload over 125 bytes
		std::vector<uint8_t> f = masked(ws::Opcode::ping, true, std::string(126, 'p'));
		d.feed(f, 1000);
		CHECK(d.error_code == 1002);
	}
	{
		Decoder d; // a continuation without a started message
		d.feed(masked(ws::Opcode::continuation, true, "stray"), 1000);
		CHECK(d.error_code == 1002);
	}
	{
		Decoder d; // an unmasked client frame is a protocol error
		uint8_t header[ws::kMaxHeader];
		size_t n = ws::encode_header(header, ws::Opcode::text, true, 2, nullptr);
		std::vector<uint8_t> f(header, header + n);
		f.push_back('h');
		f.push_back('i');
		d.feed(f, 1000);
		CHECK(d.error_code == 1002);
	}
}

TEST_CASE("WebProbe.AuthLifeCycleAcrossOnboardingClearingTokensAndSources") {
	hc_test::ScopedCapture capture;
	AuthRig r;
	r.auth.start();
	CHECK_FALSE(r.auth.has_password());
	CHECK(r.login("whatever1", "192.0.2.5") != AuthStatus::ok); // nothing to log in to yet
	std::string first;
	REQUIRE(r.onboard("correct horse", &first) == AuthStatus::ok);
	CHECK(r.onboard("again please") != AuthStatus::ok); // onboarding happens once
	hc::SessionRef ref = r.auth.session_from_cookie(("hc_session=" + first).c_str());
	CHECK(ref.valid());
	// A token outlives sessions; after clearing the password sessions are gone but tokens stay.
	char token[Auth::kTokenText];
	REQUIRE(r.auth.create_token("ehome", token) == AuthStatus::ok);
	CHECK(std::string(token).rfind("hct_", 0) == 0);
	std::string header = std::string("Bearer ") + token;
	int slot = r.auth.token_from_authorization(header.c_str());
	CHECK(slot >= 0);
	REQUIRE(r.auth.clear_password() == AuthStatus::ok);
	CHECK_FALSE(r.auth.session_alive(ref));
	CHECK_FALSE(r.auth.has_password());
	CHECK(r.auth.token_from_authorization(header.c_str()) >= 0);
	CHECK(r.auth.token_count() == 1);
	// Onboarding is open again; rate limits are per source.
	REQUIRE(r.onboard("new password 1") == AuthStatus::ok);
	for (int i = 0; i < 5; ++i) {
		CHECK(r.login("wrong", "192.0.2.7") != AuthStatus::ok);
	}
	CHECK(r.login("new password 1", "192.0.2.7") != AuthStatus::ok); // blocked now, even if correct
	CHECK(r.login("new password 1", "192.0.2.8") == AuthStatus::ok); // another source is not
	r.clock.advance(Auth::kBlockBaseMs + 1);
	CHECK(r.login("new password 1", "192.0.2.7") == AuthStatus::ok); // the block has ended
	// Nine logins: the least recently used session is replaced, the recently touched one survives.
	std::vector<std::string> sessions;
	for (int i = 0; i < 8; ++i) {
		std::string s;
		REQUIRE(r.login("new password 1", "192.0.2.9", &s) == AuthStatus::ok);
		sessions.push_back(s);
		r.clock.advance(1000);
	}
	hc::SessionRef oldest = r.auth.session_from_cookie(("hc_session=" + sessions[0]).c_str());
	r.auth.touch(oldest); // used just now: no longer the least recently used
	std::string ninth;
	REQUIRE(r.login("new password 1", "192.0.2.9", &ninth) == AuthStatus::ok);
	CHECK(r.auth.session_alive(oldest));
	CHECK_FALSE(r.auth.session_from_cookie(("hc_session=" + sessions[1]).c_str()).valid());
	CHECK(r.auth.session_count() == Auth::kSessionSlots);
	// Revoking the token ends its authorization; a garbage header never matches.
	REQUIRE(r.auth.revoke_token("ehome") == AuthStatus::ok);
	CHECK(r.auth.token_from_authorization(header.c_str()) < 0);
	CHECK(r.auth.token_from_authorization("Bearer hct_") < 0);
	CHECK(r.auth.token_from_authorization("Basic abc") < 0);
	CHECK(capture.sink.count("E hc.auth") + capture.sink.count("W hc.auth") >= 1); // the blocks were logged
}

TEST_CASE("WebProbe.LogRingSequenceAccountingAcrossWraps") {
	hc_test::FakeClock clock;
	hc::LogRing ring(&clock);
	for (int i = 0; i < 500; ++i) {
		clock.advance(10);
		ring.append(hc::LogLevel::info, "hc.t", (std::string("line ") + std::to_string(i)).c_str());
	}
	CHECK(ring.last_seq() == 500);
	CHECK(ring.first_seq() > 1); // the oldest lines are gone: the gap is first_seq() - 1 lines
	CHECK(ring.dropped() == 0);  // dropped() counts lines that could not be stored at all, not overwritten ones
	struct Collect : hc::LogLineVisitor {
		std::vector<uint32_t> seqs;
		bool line(const hc::LogLine& l) override {
			seqs.push_back(l.seq);
			return true;
		}
	} all;
	size_t n = ring.read(0, all);
	CHECK(n == all.seqs.size());
	CHECK(n == ring.count());
	for (size_t i = 1; i < all.seqs.size(); ++i) {
		CHECK(all.seqs[i] == all.seqs[i - 1] + 1); // contiguous
	}
	CHECK(all.seqs.back() == 500);
	// Reading after a sequence that was dropped returns everything still there; after the last, nothing.
	Collect after;
	CHECK(ring.read(1, after) == n);
	Collect none;
	CHECK(ring.read(500, none) == 0);
	// A visitor that stops early stops the read.
	struct Two : hc::LogLineVisitor {
		int seen = 0;
		bool line(const hc::LogLine&) override { return ++seen < 2; }
	} two;
	ring.read(0, two);
	CHECK(two.seen == 2);
}
