/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_AUTH_H
#define HC_AUTH_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/clock.h"
#include "hc/platform/crypto.h"
#include "hc/platform/settings.h"
#include "hc/sha.h"

namespace hc {

/// Outcome of an Auth call; auth_status_code() gives the API's error code, auth_status_text() the
/// English reason.
enum class AuthStatus : uint8_t {
	ok,
	no_password,     ///< No device password yet: onboarding first.
	password_set,    ///< Onboarding refused: a password exists.
	wrong_password,
	rate_limited,    ///< Too many failures from this source: wait retry_after.
	password_invalid, ///< Too short, too long or with control characters.
	password_changed, ///< The password changed while the hash was computed: retry.
	storage_error,
	no_random,       ///< The platform has no secure random source.
	name_invalid,    ///< Token name not [A-Za-z0-9._-]{1,24}.
	name_taken,
	not_found,
	capacity,        ///< All token slots used.
};

/// API error code ("auth.wrong_password", ...), also the UI's translation key.
const char* auth_status_code(AuthStatus status);
/// English reason.
const char* auth_status_text(AuthStatus status);

/// A session as the API refers to it: the slot and its generation (a logged-out or expired slot
/// gets a new generation, so an old reference never matches the next session in that slot).
struct SessionRef {
	int8_t slot = -1;
	uint32_t generation = 0;
	bool valid() const { return slot >= 0; }
};

/// The parameters of a password hash (PBKDF2-HMAC-SHA256).
struct HashParams {
	uint8_t salt[16] = {};
	uint32_t iterations = 0;
};

/// The web authentication's decisions and state (doc/web.md):
///
/// - The device password, set once by onboarding; stored as
///   `pbkdf2-sha256$<iterations>$<salt hex>$<key hex>` under the setting web.pw. Hashing takes long
///   on the device, so it happens outside: begin_*() hands out the parameters, the platform derives
///   the key off the event loop (Crypto::pbkdf2_sha256), finish_*() decides.
/// - Browser sessions: kSessionSlots slots in RAM with a random 128-bit id (the cookie), expiring
///   after kSessionIdleMs without use and at every restart; when all are taken the least recently
///   used one is replaced.
/// - Remembered sessions ("stay logged in", homecontrol #53): kRememberedSlots slots of their own,
///   stored as the SHA-256 of the id under web.sess.0 .. web.sess.3 (`<order>$<hash hex>`, the order
///   a creation counter). They have no idle expiry and survive restarts; they end at logout, with
///   clear_sessions() and clear_password(). The settings are written only when one is created or
///   ended, never on use; a fifth one replaces the oldest by creation order.
/// - Bearer tokens for machines: "hct_" and 32 hex digits, shown once at creation, stored as their
///   SHA-256 under web.tok.0 .. web.tok.7 (`<name>$<hash hex>`), revocable by name.
/// - Rate limiting: per source address, kFreeFailures failed logins are free; each further one
///   blocks the source for kBlockBaseMs, doubling up to kBlockMaxMs; a success clears it.
///
/// Threading: the event loop's thread.
class Auth {
public:
	static constexpr uint32_t kIterations = 10000;
	static constexpr size_t kKeySize = 32;
	static constexpr size_t kMinPassword = 8;
	static constexpr size_t kMaxPassword = 64;

	static constexpr size_t kSessionSlots = 8;
	static constexpr uint64_t kSessionIdleMs = 24ull * 60 * 60 * 1000;
	static constexpr size_t kSessionIdBytes = 16;
	/// The cookie's value: 32 hex digits.
	static constexpr size_t kSessionText = kSessionIdBytes * 2 + 1;
	static constexpr const char* kCookieName = "hc_session";
	/// The Max-Age of a remembered session's cookie: 10 years (the device decides validity).
	static constexpr uint32_t kRememberedCookieAgeS = 10u * 365 * 24 * 60 * 60;

	static constexpr size_t kRememberedSlots = 4;

	static constexpr size_t kMaxTokens = 8;
	static constexpr size_t kMaxTokenName = 24;
	static constexpr const char* kTokenPrefix = "hct_";
	/// A token: the prefix and 32 hex digits, plus the NUL.
	static constexpr size_t kTokenText = 4 + 32 + 1;

	static constexpr size_t kRateSlots = 8;
	static constexpr uint32_t kFreeFailures = 5;
	static constexpr uint64_t kBlockBaseMs = 30000;
	static constexpr uint64_t kBlockMaxMs = 15 * 60 * 1000;
	static constexpr size_t kMaxSource = 48;

	/// Persisted setting keys (never rename).
	static constexpr const char* kPasswordKey = "web.pw";
	static constexpr const char* kTokenKeyPrefix = "web.tok.";
	static constexpr const char* kRememberedKeyPrefix = "web.sess.";

	Auth(Settings& settings, const Clock& clock, Crypto& crypto);

	/// Reads the password record, the tokens and the remembered sessions from the settings (an
	/// unreadable record is logged and treated as absent).
	void start();

	bool has_password() const { return has_password_; }

	/// A password the device accepts: kMinPassword..kMaxPassword bytes, no control characters.
	static bool password_valid(const char* password, size_t length, const char** error);

	// ---- Onboarding and login (two phases around the hashing) --------------------------------

	/// Onboarding: refused if a password exists; otherwise fresh parameters for the new hash.
	AuthStatus begin_onboarding(HashParams& out);
	/// Stores the derived key as the password and opens a session (a remembered one with `remember`).
	AuthStatus finish_onboarding(const HashParams& params, const uint8_t key[kKeySize], const char* source,
		char session[kSessionText], bool remember = false);

	/// Login: refused without a password or while the source is blocked (`retry_after_s` set);
	/// otherwise the stored parameters.
	AuthStatus begin_login(const char* source, HashParams& out, uint32_t& retry_after_s);
	/// Compares the derived key; on success opens a session (a remembered one with `remember`), on
	/// failure counts it for the source.
	AuthStatus finish_login(const char* source, const HashParams& params, const uint8_t key[kKeySize],
		char session[kSessionText], uint32_t& retry_after_s, uint32_t& failures, bool remember = false);

	/// Erases the password and ends every session, the remembered ones included (console `web
	/// password clear`): the device returns to onboarding. Tokens stay.
	AuthStatus clear_password();

	// ---- Sessions ------------------------------------------------------------------------------

	/// The session of a Cookie header value ("a=b; hc_session=<hex>; ..."); invalid if none or
	/// expired. A hit counts as use (in RAM only: a remembered session is never written on use).
	/// The slot of a reference is 0..kSessionSlots-1 for a RAM session, kSessionSlots.. for a
	/// remembered one.
	SessionRef session_from_cookie(const char* cookie);
	/// Whether a session reference is still alive (not logged out, replaced or expired).
	bool session_alive(const SessionRef& ref) const;
	/// Counts as use (keeps a RAM session from expiring; nothing for a remembered one).
	void touch(const SessionRef& ref);
	/// Ends the session; a remembered one is erased from the settings (storage_error if that failed:
	/// it is ended until the restart, and logged).
	AuthStatus end_session(const SessionRef& ref);
	/// Ends every session, RAM and remembered ("log out everywhere"; console `web session clear`).
	/// `ended` is how many were alive. storage_error if a remembered one could not be erased.
	AuthStatus clear_sessions(size_t* ended = nullptr);
	/// Alive sessions of both kinds.
	size_t session_count() const;
	/// Remembered sessions.
	size_t remembered_count() const;

	// ---- Tokens --------------------------------------------------------------------------------

	static bool token_name_valid(const char* name);
	/// Creates a token named `name` and writes its text into `out` (shown once, never stored).
	AuthStatus create_token(const char* name, char out[kTokenText]);
	AuthStatus revoke_token(const char* name);
	/// The token of an Authorization header value ("Bearer hct_..."); its slot or -1.
	int token_from_authorization(const char* authorization) const;
	/// Whether the token slot still holds the token with this hash (not revoked).
	bool token_alive(int slot, const uint8_t hash[Sha256::kDigestSize]) const;
	/// The stored hash of a slot (for token_alive()).
	const uint8_t* token_hash(int slot) const;
	size_t token_count() const;
	/// Name of slot `i` (0..kMaxTokens-1), nullptr if the slot is free.
	const char* token_name(size_t i) const;

	/// Writes `bytes` (at most 32) secure random bytes as 2 * `bytes` lower-case hex digits and a NUL
	/// into `out` (e.g. the one-time token of a firmware upload). False if the platform has no
	/// secure random source or `bytes` is out of range.
	bool random_hex(size_t bytes, char* out);

	/// Formats and parses the stored password record (exposed for the tests).
	static bool format_record(const HashParams& params, const uint8_t key[kKeySize], char* out, size_t size);
	static bool parse_record(const char* text, HashParams& params, uint8_t key[kKeySize]);

private:
	struct Session {
		uint8_t id[kSessionIdBytes] = {};
		uint64_t last_used_ms = 0;
		uint32_t generation = 0;
		bool used = false;
	};

	/// A remembered session: the SHA-256 of its id, its creation order (persisted) and its
	/// generation (RAM only, like a RAM slot's).
	struct Remembered {
		uint8_t hash[Sha256::kDigestSize] = {};
		uint32_t order = 0;
		uint32_t generation = 0;
		bool used = false;
	};

	struct Token {
		char name[kMaxTokenName + 1] = {};
		uint8_t hash[Sha256::kDigestSize] = {};
		bool used = false;
	};

	struct RateSlot {
		char source[kMaxSource] = {};
		uint32_t failures = 0;
		uint64_t blocked_until_ms = 0;
		uint64_t last_ms = 0;
		bool used = false;
	};

	AuthStatus open_session(char session[kSessionText], bool remember);
	AuthStatus open_remembered(char session[kSessionText]);
	AuthStatus end_remembered(size_t i);
	AuthStatus clear_remembered();
	void expire_sessions();
	RateSlot* rate_slot(const char* source, bool create);
	static void token_key(size_t slot, char* out, size_t size);
	static void remembered_key(size_t slot, char* out, size_t size);

	Settings& settings_;
	const Clock& clock_;
	Crypto& crypto_;
	bool has_password_ = false;
	HashParams password_params_;
	uint8_t password_key_[kKeySize] = {};
	Session sessions_[kSessionSlots];
	Remembered remembered_[kRememberedSlots];
	/// The order the next remembered session gets (one above the highest stored).
	uint32_t next_order_ = 1;
	Token tokens_[kMaxTokens];
	RateSlot rate_[kRateSlots];
};

} // namespace hc

#endif // HC_AUTH_H
