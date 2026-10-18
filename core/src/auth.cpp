/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/auth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.auth";
constexpr const char* kScheme = "pbkdf2-sha256";

} // namespace

const char* auth_status_code(AuthStatus status) {
	switch (status) {
	case AuthStatus::ok: return "ok";
	case AuthStatus::no_password: return "auth.onboarding_required";
	case AuthStatus::password_set: return "auth.password_set";
	case AuthStatus::wrong_password: return "auth.wrong_password";
	case AuthStatus::rate_limited: return "auth.rate_limited";
	case AuthStatus::password_invalid: return "auth.password_invalid";
	case AuthStatus::password_changed: return "auth.password_changed";
	case AuthStatus::storage_error: return "auth.storage_error";
	case AuthStatus::no_random: return "auth.no_random";
	case AuthStatus::name_invalid: return "auth.token_name_invalid";
	case AuthStatus::name_taken: return "auth.token_name_taken";
	case AuthStatus::not_found: return "auth.token_not_found";
	case AuthStatus::capacity: return "auth.token_capacity";
	}
	return "auth.unknown";
}

const char* auth_status_text(AuthStatus status) {
	switch (status) {
	case AuthStatus::ok: return "ok";
	case AuthStatus::no_password: return "no device password is set yet: set one on the onboarding page";
	case AuthStatus::password_set: return "the device password is already set";
	case AuthStatus::wrong_password: return "wrong password";
	case AuthStatus::rate_limited: return "too many failed logins from this address: wait and try again";
	case AuthStatus::password_invalid: return "the password must have 8 to 64 characters and no control characters";
	case AuthStatus::password_changed: return "the password changed meanwhile: try again";
	case AuthStatus::storage_error: return "the settings could not be written";
	case AuthStatus::no_random: return "the device has no secure random source";
	case AuthStatus::name_invalid: return "a token name has 1 to 24 characters of A-Z a-z 0-9 . _ -";
	case AuthStatus::name_taken: return "a token with this name exists: revoke it first";
	case AuthStatus::not_found: return "no token with this name";
	case AuthStatus::capacity: return "all 8 token slots are used: revoke one first";
	}
	return "unknown";
}

Auth::Auth(Settings& settings, const Clock& clock, Crypto& crypto)
	: settings_(settings), clock_(clock), crypto_(crypto) {}

void Auth::token_key(size_t slot, char* out, size_t size) {
	std::snprintf(out, size, "%s%u", kTokenKeyPrefix, static_cast<unsigned>(slot));
}

void Auth::start() {
	char text[Settings::kMaxStringLength + 1];
	SettingsStatus status = settings_.get_string(kPasswordKey, text, sizeof(text));
	has_password_ = false;
	if (status == SettingsStatus::ok) {
		if (parse_record(text, password_params_, password_key_)) {
			has_password_ = true;
		} else {
			Log::error(kTag, "stored device password (%s) unreadable: treated as absent, onboarding is open",
				kPasswordKey);
		}
	} else if (status != SettingsStatus::not_found) {
		Log::error(kTag, "device password (%s) not readable (%s): onboarding is open", kPasswordKey,
			settings_status_name(status));
	}
	for (size_t i = 0; i < kMaxTokens; ++i) {
		tokens_[i] = Token{};
		char key[16];
		token_key(i, key, sizeof(key));
		status = settings_.get_string(key, text, sizeof(text));
		if (status == SettingsStatus::not_found) {
			continue;
		}
		const char* dollar = status == SettingsStatus::ok ? std::strchr(text, '$') : nullptr;
		size_t name_length = dollar != nullptr ? static_cast<size_t>(dollar - text) : 0;
		if (dollar == nullptr || name_length == 0 || name_length > kMaxTokenName ||
			!from_hex(dollar + 1, tokens_[i].hash, Sha256::kDigestSize)) {
			Log::error(kTag, "token %s unreadable: ignored (revoke it or create it again)", key);
			continue;
		}
		std::memcpy(tokens_[i].name, text, name_length);
		tokens_[i].name[name_length] = '\0';
		tokens_[i].used = true;
	}
	Log::info(kTag, "web authentication: %s, %u token(s)", has_password_ ? "device password set" : "no device password: onboarding open",
		static_cast<unsigned>(token_count()));
}

bool Auth::password_valid(const char* password, size_t length, const char** error) {
	const char* dummy;
	if (error == nullptr) {
		error = &dummy;
	}
	if (password == nullptr || length < kMinPassword || length > kMaxPassword) {
		*error = auth_status_text(AuthStatus::password_invalid);
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = static_cast<unsigned char>(password[i]);
		if (c < 0x20 || c == 0x7F) {
			*error = auth_status_text(AuthStatus::password_invalid);
			return false;
		}
	}
	return true;
}

bool Auth::format_record(const HashParams& params, const uint8_t key[kKeySize], char* out, size_t size) {
	char salt[sizeof(params.salt) * 2 + 1];
	char hash[kKeySize * 2 + 1];
	to_hex(params.salt, sizeof(params.salt), salt);
	to_hex(key, kKeySize, hash);
	int n = std::snprintf(out, size, "%s$%u$%s$%s", kScheme, static_cast<unsigned>(params.iterations), salt, hash);
	return n > 0 && static_cast<size_t>(n) < size;
}

bool Auth::parse_record(const char* text, HashParams& params, uint8_t key[kKeySize]) {
	size_t scheme = std::strlen(kScheme);
	if (std::strncmp(text, kScheme, scheme) != 0 || text[scheme] != '$') {
		return false;
	}
	const char* p = text + scheme + 1;
	char* end = nullptr;
	unsigned long iterations = std::strtoul(p, &end, 10);
	if (end == p || *end != '$' || iterations == 0 || iterations > 10000000ul) {
		return false;
	}
	p = end + 1;
	const char* dollar = std::strchr(p, '$');
	if (dollar == nullptr || dollar - p != static_cast<long>(sizeof(params.salt) * 2)) {
		return false;
	}
	char salt[sizeof(params.salt) * 2 + 1];
	std::memcpy(salt, p, sizeof(salt) - 1);
	salt[sizeof(salt) - 1] = '\0';
	if (!from_hex(salt, params.salt, sizeof(params.salt)) || !from_hex(dollar + 1, key, kKeySize)) {
		return false;
	}
	params.iterations = static_cast<uint32_t>(iterations);
	return true;
}

AuthStatus Auth::begin_onboarding(HashParams& out) {
	if (has_password_) {
		return AuthStatus::password_set;
	}
	if (!crypto_.random(out.salt, sizeof(out.salt))) {
		return AuthStatus::no_random;
	}
	out.iterations = kIterations;
	return AuthStatus::ok;
}

AuthStatus Auth::finish_onboarding(const HashParams& params, const uint8_t key[kKeySize], const char* source,
	char session[kSessionText]) {
	if (has_password_) {
		return AuthStatus::password_set;
	}
	char record[Settings::kMaxStringLength + 1];
	if (params.iterations == 0 || !format_record(params, key, record, sizeof(record))) {
		return AuthStatus::password_invalid;
	}
	SettingsStatus status = settings_.set_string(kPasswordKey, record);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "device password not stored: %s", settings_status_name(status));
		return AuthStatus::storage_error;
	}
	password_params_ = params;
	std::memcpy(password_key_, key, kKeySize);
	has_password_ = true;
	Log::warn(kTag, "device password set (onboarding from %s)", source != nullptr ? source : "?");
	return open_session(session);
}

Auth::RateSlot* Auth::rate_slot(const char* source, bool create) {
	if (source == nullptr) {
		source = "";
	}
	RateSlot* oldest = nullptr;
	for (RateSlot& slot : rate_) {
		if (slot.used && std::strncmp(slot.source, source, kMaxSource - 1) == 0) {
			return &slot;
		}
		if (oldest == nullptr || !slot.used || (oldest->used && slot.last_ms < oldest->last_ms)) {
			oldest = &slot;
		}
	}
	if (!create) {
		return nullptr;
	}
	*oldest = RateSlot{};
	std::snprintf(oldest->source, sizeof(oldest->source), "%s", source);
	oldest->used = true;
	return oldest;
}

AuthStatus Auth::begin_login(const char* source, HashParams& out, uint32_t& retry_after_s) {
	retry_after_s = 0;
	if (!has_password_) {
		return AuthStatus::no_password;
	}
	uint64_t now = clock_.monotonic_ms();
	RateSlot* slot = rate_slot(source, false);
	if (slot != nullptr && slot->blocked_until_ms > now) {
		retry_after_s = static_cast<uint32_t>((slot->blocked_until_ms - now + 999) / 1000);
		return AuthStatus::rate_limited;
	}
	out = password_params_;
	return AuthStatus::ok;
}

AuthStatus Auth::finish_login(const char* source, const HashParams& params, const uint8_t key[kKeySize],
	char session[kSessionText], uint32_t& retry_after_s, uint32_t& failures) {
	retry_after_s = 0;
	failures = 0;
	if (!has_password_) {
		return AuthStatus::no_password;
	}
	uint64_t now = clock_.monotonic_ms();
	RateSlot* slot = rate_slot(source, false);
	if (slot != nullptr && slot->blocked_until_ms > now) {
		retry_after_s = static_cast<uint32_t>((slot->blocked_until_ms - now + 999) / 1000);
		return AuthStatus::rate_limited;
	}
	if (params.iterations != password_params_.iterations ||
		std::memcmp(params.salt, password_params_.salt, sizeof(params.salt)) != 0) {
		return AuthStatus::password_changed;
	}
	if (constant_time_equal(key, password_key_, kKeySize)) {
		if (slot != nullptr) {
			*slot = RateSlot{};
		}
		Log::info(kTag, "login from %s", source != nullptr ? source : "?");
		return open_session(session);
	}
	slot = rate_slot(source, true);
	slot->failures++;
	slot->last_ms = now;
	failures = slot->failures;
	if (slot->failures >= kFreeFailures) {
		uint32_t extra = slot->failures - kFreeFailures;
		uint64_t block = kBlockBaseMs;
		for (uint32_t i = 0; i < extra && block < kBlockMaxMs; ++i) {
			block *= 2;
		}
		if (block > kBlockMaxMs) {
			block = kBlockMaxMs;
		}
		slot->blocked_until_ms = now + block;
		retry_after_s = static_cast<uint32_t>(block / 1000);
		Log::warn(kTag, "login from %s failed (%u failures): blocked for %u s", slot->source,
			static_cast<unsigned>(slot->failures), static_cast<unsigned>(retry_after_s));
	} else {
		Log::warn(kTag, "login from %s failed (%u of %u free attempts)", slot->source,
			static_cast<unsigned>(slot->failures), static_cast<unsigned>(kFreeFailures));
	}
	return AuthStatus::wrong_password;
}

AuthStatus Auth::clear_password() {
	SettingsStatus status = settings_.erase(kPasswordKey);
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		return AuthStatus::storage_error;
	}
	has_password_ = false;
	std::memset(password_key_, 0, sizeof(password_key_));
	for (Session& s : sessions_) {
		if (s.used) {
			s.used = false;
			++s.generation;
		}
	}
	Log::warn(kTag, "device password cleared, all sessions ended: onboarding is open");
	return AuthStatus::ok;
}

void Auth::expire_sessions() {
	uint64_t now = clock_.monotonic_ms();
	for (Session& s : sessions_) {
		if (s.used && now - s.last_used_ms >= kSessionIdleMs) {
			s.used = false;
			++s.generation;
		}
	}
}

AuthStatus Auth::open_session(char session[kSessionText]) {
	expire_sessions();
	Session* target = nullptr;
	for (Session& s : sessions_) {
		if (!s.used) {
			target = &s;
			break;
		}
		if (target == nullptr || s.last_used_ms < target->last_used_ms) {
			target = &s;
		}
	}
	if (target->used) {
		Log::info(kTag, "all %u sessions in use: the least recently used one ends", static_cast<unsigned>(kSessionSlots));
	}
	if (!crypto_.random(target->id, sizeof(target->id))) {
		return AuthStatus::no_random;
	}
	++target->generation;
	target->used = true;
	target->last_used_ms = clock_.monotonic_ms();
	to_hex(target->id, sizeof(target->id), session);
	return AuthStatus::ok;
}

SessionRef Auth::session_from_cookie(const char* cookie) {
	SessionRef ref;
	if (cookie == nullptr) {
		return ref;
	}
	size_t name_length = std::strlen(kCookieName);
	const char* p = cookie;
	while (*p != '\0') {
		while (*p == ' ' || *p == ';') {
			++p;
		}
		if (std::strncmp(p, kCookieName, name_length) == 0 && p[name_length] == '=') {
			const char* value = p + name_length + 1;
			char text[kSessionText];
			size_t n = 0;
			while (value[n] != '\0' && value[n] != ';' && value[n] != ' ' && n < sizeof(text) - 1) {
				text[n] = value[n];
				++n;
			}
			text[n] = '\0';
			uint8_t id[kSessionIdBytes];
			if (n == kSessionText - 1 && from_hex(text, id, sizeof(id))) {
				expire_sessions();
				for (size_t i = 0; i < kSessionSlots; ++i) {
					Session& s = sessions_[i];
					if (s.used && constant_time_equal(s.id, id, sizeof(id))) {
						s.last_used_ms = clock_.monotonic_ms();
						ref.slot = static_cast<int8_t>(i);
						ref.generation = s.generation;
						return ref;
					}
				}
			}
		}
		const char* next = std::strchr(p, ';');
		if (next == nullptr) {
			break;
		}
		p = next;
	}
	return ref;
}

bool Auth::session_alive(const SessionRef& ref) const {
	if (!ref.valid() || static_cast<size_t>(ref.slot) >= kSessionSlots) {
		return false;
	}
	const Session& s = sessions_[ref.slot];
	return s.used && s.generation == ref.generation && clock_.monotonic_ms() - s.last_used_ms < kSessionIdleMs;
}

void Auth::touch(const SessionRef& ref) {
	if (session_alive(ref)) {
		sessions_[ref.slot].last_used_ms = clock_.monotonic_ms();
	}
}

void Auth::end_session(const SessionRef& ref) {
	if (session_alive(ref)) {
		sessions_[ref.slot].used = false;
		++sessions_[ref.slot].generation;
	}
}

size_t Auth::session_count() const {
	size_t n = 0;
	uint64_t now = clock_.monotonic_ms();
	for (const Session& s : sessions_) {
		if (s.used && now - s.last_used_ms < kSessionIdleMs) {
			++n;
		}
	}
	return n;
}

bool Auth::token_name_valid(const char* name) {
	if (name == nullptr) {
		return false;
	}
	size_t n = std::strlen(name);
	if (n == 0 || n > kMaxTokenName) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = name[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
			c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

AuthStatus Auth::create_token(const char* name, char out[kTokenText]) {
	out[0] = '\0';
	if (!token_name_valid(name)) {
		return AuthStatus::name_invalid;
	}
	int free_slot = -1;
	for (size_t i = 0; i < kMaxTokens; ++i) {
		if (tokens_[i].used && std::strcmp(tokens_[i].name, name) == 0) {
			return AuthStatus::name_taken;
		}
		if (!tokens_[i].used && free_slot < 0) {
			free_slot = static_cast<int>(i);
		}
	}
	if (free_slot < 0) {
		return AuthStatus::capacity;
	}
	uint8_t secret[16];
	if (!crypto_.random(secret, sizeof(secret))) {
		return AuthStatus::no_random;
	}
	char text[kTokenText];
	std::memcpy(text, kTokenPrefix, 4);
	to_hex(secret, sizeof(secret), text + 4);
	Token token;
	std::snprintf(token.name, sizeof(token.name), "%s", name);
	Sha256::hash(text, std::strlen(text), token.hash);
	token.used = true;
	char hash_hex[Sha256::kDigestSize * 2 + 1];
	to_hex(token.hash, sizeof(token.hash), hash_hex);
	char record[Settings::kMaxStringLength + 1];
	std::snprintf(record, sizeof(record), "%s$%s", name, hash_hex);
	char key[16];
	token_key(static_cast<size_t>(free_slot), key, sizeof(key));
	SettingsStatus status = settings_.set_string(key, record);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "token %s not stored: %s", name, settings_status_name(status));
		return AuthStatus::storage_error;
	}
	tokens_[free_slot] = token;
	std::memcpy(out, text, kTokenText);
	Log::warn(kTag, "API token \"%s\" created", name);
	return AuthStatus::ok;
}

AuthStatus Auth::revoke_token(const char* name) {
	for (size_t i = 0; i < kMaxTokens; ++i) {
		if (tokens_[i].used && name != nullptr && std::strcmp(tokens_[i].name, name) == 0) {
			char key[16];
			token_key(i, key, sizeof(key));
			SettingsStatus status = settings_.erase(key);
			if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
				return AuthStatus::storage_error;
			}
			tokens_[i] = Token{};
			Log::warn(kTag, "API token \"%s\" revoked", name);
			return AuthStatus::ok;
		}
	}
	return AuthStatus::not_found;
}

int Auth::token_from_authorization(const char* authorization) const {
	if (authorization == nullptr || std::strncmp(authorization, "Bearer ", 7) != 0) {
		return -1;
	}
	const char* token = authorization + 7;
	while (*token == ' ') {
		++token;
	}
	size_t length = std::strlen(token);
	while (length > 0 && token[length - 1] == ' ') {
		--length;
	}
	if (length != kTokenText - 1 || std::strncmp(token, kTokenPrefix, 4) != 0) {
		return -1;
	}
	uint8_t hash[Sha256::kDigestSize];
	Sha256::hash(token, length, hash);
	int found = -1;
	for (size_t i = 0; i < kMaxTokens; ++i) {
		// Compare all slots: the time does not tell which one matched.
		if (tokens_[i].used && constant_time_equal(tokens_[i].hash, hash, sizeof(hash))) {
			found = static_cast<int>(i);
		}
	}
	return found;
}

bool Auth::token_alive(int slot, const uint8_t hash[Sha256::kDigestSize]) const {
	return slot >= 0 && static_cast<size_t>(slot) < kMaxTokens && tokens_[slot].used &&
		std::memcmp(tokens_[slot].hash, hash, Sha256::kDigestSize) == 0;
}

const uint8_t* Auth::token_hash(int slot) const {
	return slot >= 0 && static_cast<size_t>(slot) < kMaxTokens ? tokens_[slot].hash : nullptr;
}

size_t Auth::token_count() const {
	size_t n = 0;
	for (const Token& t : tokens_) {
		n += t.used ? 1 : 0;
	}
	return n;
}

const char* Auth::token_name(size_t i) const {
	return i < kMaxTokens && tokens_[i].used ? tokens_[i].name : nullptr;
}

bool Auth::random_hex(size_t bytes, char* out) {
	uint8_t secret[32];
	if (bytes == 0 || bytes > sizeof(secret) || !crypto_.random(secret, bytes)) {
		return false;
	}
	to_hex(secret, bytes, out);
	std::memset(secret, 0, sizeof(secret));
	return true;
}

} // namespace hc
