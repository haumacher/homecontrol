/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/language_packs.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/sha.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.i18n";

bool is_hex_digit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// A plain file name: [A-Za-z0-9._-], not starting with a dot (no directory, no "..").
bool plain_file_name(const char* name) {
	if (name[0] == '\0' || name[0] == '.') {
		return false;
	}
	for (const char* p = name; *p != '\0'; ++p) {
		char c = *p;
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
			c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

/// Copies `version` without its build metadata ("+...").
void release_of(const char* version, char* out, size_t size) {
	size_t length = 0;
	while (version[length] != '\0' && version[length] != '+' && length + 1 < size) {
		out[length] = version[length];
		++length;
	}
	out[length] = '\0';
}

/// Collects up to kBatch pack blobs other than `keep` (the visitor must not modify the store).
class PackCollector : public BlobVisitor {
public:
	static constexpr size_t kBatch = 8;

	explicit PackCollector(const char* keep) : keep_(keep) {}

	void blob(const char* name, size_t) override {
		size_t prefix = std::strlen(LanguagePacks::kBlobPrefix);
		if (count_ >= kBatch || std::strncmp(name, LanguagePacks::kBlobPrefix, prefix) != 0 ||
			(keep_ != nullptr && std::strcmp(name, keep_) == 0)) {
			return;
		}
		std::snprintf(names_[count_++], sizeof(names_[0]), "%s", name);
	}

	size_t count() const { return count_; }
	const char* name(size_t i) const { return names_[i]; }

private:
	const char* keep_;
	char names_[kBatch][BlobStore::kMaxNameLength + 1] = {};
	size_t count_ = 0;
};

/// Whether `lang` ("de" or "de-AT") has the base language `base` ("de"): the same primary subtag.
bool has_base(const char* lang, const char* base) {
	size_t n = std::strlen(base);
	return std::strchr(base, '-') == nullptr && std::strncmp(lang, base, n) == 0 && (lang[n] == '\0' || lang[n] == '-');
}

/// Whether `lang` is the UI's built-in base language or a regional variant of it ("en", "en-GB").
bool is_builtin(const char* lang) {
	return has_base(lang, setting::kDefaultUiLang);
}

} // namespace

const char* language_pack_state_name(LanguagePackState state) {
	switch (state) {
		case LanguagePackState::builtin: return "builtin";
		case LanguagePackState::waiting: return "waiting";
		case LanguagePackState::fetching: return "fetching";
		case LanguagePackState::installed: return "installed";
		case LanguagePackState::not_offered: return "not_offered";
		case LanguagePackState::failed: return "failed";
	}
	return "?";
}

LanguagePacks::LanguagePacks(EventLoop& loop, const TimeSource& time, const Settings& settings,
	const Connectivity& connectivity, HttpFetcher& fetcher, BlobStore& store, const char* version)
	: loop_(loop),
	  time_(time),
	  settings_(settings),
	  connectivity_(connectivity),
	  fetcher_(fetcher),
	  store_(store),
	  backoff_(kRetryInitialMs, kRetryCapMs) {
	release_of(version, release_, sizeof(release_));
	std::snprintf(lang_, sizeof(lang_), "%s", setting::kDefaultUiLang);
}

LanguagePacks::~LanguagePacks() {
	loop_.cancel(timer_);
	drop_transfer();
}

bool LanguagePacks::blob_name(const char* lang, char* out, size_t size) {
	if (!ui_lang_valid(lang)) {
		return false;
	}
	int n = std::snprintf(out, size, "%s%s.json.gz", kBlobPrefix, lang);
	return n > 0 && static_cast<size_t>(n) < size;
}

bool LanguagePacks::manifest_url(const char* base, const char* version, char* out, size_t size) {
	size_t length = std::strlen(base);
	while (length > 0 && base[length - 1] == '/') {
		--length;
	}
	char release[Version::kMaxText + 1];
	release_of(version, release, sizeof(release));
	int n = std::snprintf(out, size, "%.*s/%s/i18n/manifest.json", static_cast<int>(length), base, release);
	return n > 0 && static_cast<size_t>(n) < size;
}

void LanguagePacks::start() {
	read_ui_lang(settings_, lang_, kTag);
	pending_ = true;
	state_ = LanguagePackState::waiting;
	std::snprintf(detail_, sizeof(detail_), "the first sync follows shortly after the start");
	arm(time_.monotonic_ms() + kStartDelayMs);
}

void LanguagePacks::language_changed() {
	read_ui_lang(settings_, lang_, kTag);
	base_pruned_ = false;
	pending_ = true;
	pack_due_ = false;
	backoff_.reset();
	state_ = LanguagePackState::waiting;
	std::snprintf(detail_, sizeof(detail_), "the language changed: a sync follows");
	if (phase_ != Phase::idle) {
		restart_ = true; // the fetch in flight is discarded when it returns
		return;
	}
	arm(time_.monotonic_ms() + kChangeDelayMs);
}

bool LanguagePacks::sync_now(const char** error) {
	if (phase_ != Phase::idle || pack_due_) {
		*error = "a sync is running";
		return false;
	}
	pending_ = true;
	backoff_.reset();
	arm(time_.monotonic_ms());
	return true;
}

void LanguagePacks::arm(uint64_t at) {
	loop_.cancel(timer_);
	timer_ = loop_.schedule_at(at, Callback::member<LanguagePacks, &LanguagePacks::timer_due>(this));
}

void LanguagePacks::timer_due() {
	timer_ = TimerHandle();
	uint64_t now = time_.monotonic_ms();
	if (phase_ != Phase::idle) {
		if (now >= deadline_) {
			phase_ = Phase::idle;
			restart_ = false;
			drop_transfer();
			fetcher_.cancel(); // a request still waiting for the fetch worker does not run any more
			fail("no answer to the fetch of %s within %u s", url_, static_cast<unsigned>(kFetchTimeoutMs / 1000));
		} else {
			arm(deadline_);
		}
		return;
	}
	if (pack_due_) {
		pack_due_ = false;
		start_pack();
		return;
	}
	if (pending_) {
		run_pass();
	}
}

const char* LanguagePacks::blocked() const {
	if (!connectivity_.is_up()) {
		return "the network is down";
	}
	if (!time_.synchronized()) {
		return "the clock is not synchronized (TLS needs the date)";
	}
	return nullptr;
}

void LanguagePacks::wait(const char* reason) {
	if (state_ != LanguagePackState::waiting || std::strcmp(detail_, reason) != 0) {
		Log::info(kTag, "language pack sync for '%s' waits: %s", lang_, reason);
	}
	state_ = LanguagePackState::waiting;
	std::snprintf(detail_, sizeof(detail_), "%s", reason);
	arm(time_.monotonic_ms() + kReadyPollMs);
}

void LanguagePacks::run_pass() {
	read_ui_lang(settings_, lang_, kTag);
	bool base_language = is_builtin(lang_);
	if (base_language && !base_pruned_) {
		prune(nullptr); // English is the UI's own base: a stored pack is dead weight
		base_pruned_ = true;
	}
	char source[Settings::kMaxStringLength + 1] = {};
	bool has_source = update_source(settings_, source, sizeof(source));
	const char* reason = blocked();
	const char* url_error = nullptr;
	if (reason == nullptr && !has_source) {
		reason = "the update channel is off: packs come from the release site";
	} else if (reason == nullptr && !update_url_valid(source, &url_error)) {
		reason = "update.url is invalid";
	}
	if (reason == nullptr && !manifest_url(source, release_, url_, sizeof(url_))) {
		reason = "the manifest URL does not fit";
	}
	if (reason != nullptr) {
		if (base_language) {
			// Nothing to download; the poll only keeps the offered languages coming once it can.
			state_ = LanguagePackState::builtin;
			std::snprintf(detail_, sizeof(detail_), "%s", reason);
			arm(time_.monotonic_ms() + kReadyPollMs);
			return;
		}
		wait(reason);
		return;
	}
	// The base of the pack paths: the manifest URL without "manifest.json".
	std::snprintf(base_, sizeof(base_), "%s", url_);
	char* slash = std::strrchr(base_, '/');
	if (slash != nullptr) {
		slash[1] = '\0';
	}
	if (fetcher_.busy()) {
		wait("the HTTP fetcher is busy");
		return;
	}
	if (manifest_ == nullptr) {
		manifest_ = new (std::nothrow) char[kMaxManifest];
		if (manifest_ == nullptr) {
			fail("no memory for the manifest (%u bytes)", static_cast<unsigned>(kMaxManifest));
			return;
		}
	}
	body_ = FetchBody(manifest_, kMaxManifest, "the manifest");
	if (!begin_fetch(url_, Phase::manifest)) {
		drop_transfer();
	}
}

void LanguagePacks::start_pack() {
	if (std::snprintf(url_, sizeof(url_), "%s%s", base_, wanted_.path) >= static_cast<int>(sizeof(url_))) {
		fail("the pack URL is longer than %u bytes", static_cast<unsigned>(HttpFetcher::kMaxUrl));
		return;
	}
	// The pack must fit beside the stored one (the replace is atomic: both exist until the rename).
	size_t free = 0;
	if (store_.free_space(free) && wanted_.size > free) {
		fail("pack %s refused: %u bytes, the blob store has %u bytes free", url_, static_cast<unsigned>(wanted_.size),
			static_cast<unsigned>(free));
		return;
	}
	begin_fetch(url_, Phase::pack);
}

bool LanguagePacks::begin_fetch(const char* url, Phase phase) {
	const char* error = "";
	abort_[0] = '\0';
	if (!fetcher_.fetch(url, *this, &error)) {
		fail("cannot fetch %s: %s", url, error);
		return false;
	}
	phase_ = phase;
	pending_ = false;
	restart_ = false;
	state_ = LanguagePackState::fetching;
	std::snprintf(detail_, sizeof(detail_), "%s", url);
	deadline_ = time_.monotonic_ms() + kFetchTimeoutMs;
	arm(deadline_);
	return true;
}

bool LanguagePacks::abort(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(abort_, sizeof(abort_), fmt, args);
	va_end(args);
	return false;
}

void LanguagePacks::drop_transfer() {
	if (writer_ != nullptr) {
		writer_->discard();
		writer_ = nullptr;
	}
	delete[] manifest_;
	manifest_ = nullptr;
	body_ = FetchBody(nullptr, 0, "the manifest");
}

bool LanguagePacks::fetch_begin(int http_status, size_t content_length) {
	if (phase_ == Phase::idle || restart_) {
		return false; // timed out, or the language changed: the end is discarded
	}
	if (http_status != 200) {
		return false; // fetch_end() names the status
	}
	if (phase_ == Phase::manifest) {
		return body_.begin(content_length) || abort("manifest %s refused: %s", url_, body_.refusal());
	}
	return begin_pack(content_length);
}

bool LanguagePacks::fetch_chunk(const char* data, size_t size) {
	if (phase_ == Phase::idle || restart_) {
		return false;
	}
	if (phase_ == Phase::manifest) {
		return body_.append(data, size) || abort("manifest %s refused: %s", url_, body_.refusal());
	}
	return take_pack(data, size);
}

bool LanguagePacks::begin_pack(size_t content_length) {
	if (content_length != FetchReceiver::kUnknownLength && content_length != wanted_.size) {
		return abort("pack %s refused: the server announces %u bytes, the manifest says %u", url_,
			static_cast<unsigned>(content_length), static_cast<unsigned>(wanted_.size));
	}
	char name[BlobStore::kMaxNameLength + 1];
	blob_name(lang_, name, sizeof(name));
	BlobStatus status = BlobStatus::ok;
	writer_ = store_.open_writer(name, wanted_.size, status);
	if (writer_ == nullptr) {
		return abort("cannot store the pack %s: %s", name, blob_status_name(status));
	}
	sha_ = Sha256();
	received_ = 0;
	return true;
}

bool LanguagePacks::take_pack(const char* data, size_t size) {
	if (writer_ == nullptr) {
		return false;
	}
	if (size > wanted_.size - received_) {
		return abort("pack %s refused: longer than the %u bytes the manifest says", url_,
			static_cast<unsigned>(wanted_.size));
	}
	BlobStatus status = writer_->append(data, size);
	if (status != BlobStatus::ok) {
		return abort("cannot store the pack %s after %u bytes: %s", url_, static_cast<unsigned>(received_),
			blob_status_name(status));
	}
	sha_.update(data, size);
	received_ += size;
	return true;
}

void LanguagePacks::fetch_end(const FetchResult& result) {
	Phase phase = phase_;
	if (phase == Phase::idle) {
		return; // timed out meanwhile (the transfer was dropped then)
	}
	phase_ = Phase::idle;
	loop_.cancel(timer_);
	timer_ = TimerHandle();
	if (restart_) {
		restart_ = false;
		drop_transfer();
		pending_ = true;
		arm(time_.monotonic_ms() + kChangeDelayMs);
		return;
	}
	if (!result.ok) {
		drop_transfer();
		if (result.aborted && abort_[0] != '\0') {
			fail("%s", abort_);
		} else {
			fail("fetch of %s failed: %s", url_,
				result.error != nullptr && result.error[0] != '\0' ? result.error : "no reason");
		}
		return;
	}
	if (phase == Phase::manifest) {
		manifest_arrived(body_.data(), body_.length());
		delete[] manifest_;
		manifest_ = nullptr;
		body_ = FetchBody(nullptr, 0, "the manifest");
	} else {
		pack_arrived();
	}
}

bool LanguagePacks::parse_manifest(const char* body, size_t length, Entry& wanted, bool& found, const char** error) {
	JsonSpan root;
	JsonSpan member;
	if (!json_parse(body, length, root, error) || root.kind != JsonSpan::Kind::object) {
		if (root.kind != JsonSpan::Kind::invalid) {
			*error = "the manifest is not a JSON object";
		}
		return false;
	}
	char version[Version::kMaxText + 1];
	if (!json_member(root, "version", member) || !json_string(member, version, sizeof(version))) {
		*error = "the manifest has no \"version\"";
		return false;
	}
	if (std::strcmp(version, release_) != 0) {
		*error = "the manifest is for another release";
		return false;
	}
	JsonSpan packs;
	if (!json_member(root, "packs", packs) || packs.kind != JsonSpan::Kind::object) {
		*error = "the manifest has no \"packs\" object";
		return false;
	}
	offer_count_ = 0;
	found = false;
	bool found_exact = false;
	const char* base_refusal = nullptr;
	*error = nullptr;
	size_t position = 0;
	JsonSpan name;
	JsonSpan value;
	while (json_next_member(packs, position, name, value)) {
		Entry entry = {};
		char code[16] = {};
		const char* refusal = nullptr;
		if (!json_string(name, code, sizeof(code)) || !ui_lang_valid(code)) {
			refusal = "not a language tag";
		} else if (value.kind != JsonSpan::Kind::object) {
			refusal = "not an object";
		} else {
			std::memcpy(entry.code, code, std::strlen(code) + 1); // a valid tag fits
			JsonSpan field;
			double size = 0;
			if (!json_member(value, "name", field) || !json_string(field, entry.name, sizeof(entry.name)) ||
				entry.name[0] == '\0') {
				refusal = "\"name\" missing or longer than 23 bytes";
			} else if (!json_member(value, "path", field) || !json_string(field, entry.path, sizeof(entry.path)) ||
				!plain_file_name(entry.path)) {
				refusal = "\"path\" must be a plain file name of [A-Za-z0-9._-] (at most 63 bytes)";
			} else if (!json_member(value, "sha256", field) || !json_string(field, entry.sha256, sizeof(entry.sha256)) ||
				std::strlen(entry.sha256) != 64) {
				refusal = "\"sha256\" must be 64 hex digits";
			} else if (!json_member(value, "size", field) || !json_number(field, size) || size < 1 ||
				size > 4294967295.0 || size != static_cast<double>(static_cast<uint32_t>(size))) {
				refusal = "\"size\" must be a whole number of bytes, at least 1";
			} else {
				for (char& c : entry.sha256) {
					if (c == '\0') {
						break;
					}
					if (!is_hex_digit(c)) {
						refusal = "\"sha256\" must be 64 hex digits";
						break;
					}
					if (c >= 'A' && c <= 'F') {
						c = static_cast<char>(c - 'A' + 'a');
					}
				}
				entry.size = static_cast<uint32_t>(size);
			}
		}
		// The pack of ui.lang itself, or, for a regional tag ("de-AT"), the pack of its base language
		// ("de") when the release has none for the region.
		bool is_wanted = std::strcmp(code, lang_) == 0;
		bool is_base = !is_wanted && has_base(lang_, code);
		if (refusal != nullptr) {
			Log::warn(kTag, "manifest entry '%s' refused: %s", code, refusal);
			if (is_wanted) {
				*error = refusal; // the pack the device needs is unusable
			} else if (is_base) {
				base_refusal = refusal;
			}
			continue;
		}
		if (offer_count_ < kMaxOffers) {
			LanguageOffer& offer = offers_[offer_count_++];
			std::snprintf(offer.code, sizeof(offer.code), "%s", entry.code);
			std::snprintf(offer.name, sizeof(offer.name), "%s", entry.name);
		}
		if (is_wanted || (is_base && !found_exact)) {
			wanted = entry;
			found = true;
			found_exact = is_wanted;
		}
	}
	if (*error == nullptr && !found && base_refusal != nullptr) {
		*error = base_refusal; // the base language's pack would have served the region
	}
	if (*error != nullptr) {
		return false;
	}
	return true;
}

void LanguagePacks::manifest_arrived(const char* body, size_t length) {
	const char* error = nullptr;
	bool found = false;
	if (!parse_manifest(body, length, wanted_, found, &error)) {
		fail("manifest %s refused: %s", url_, error != nullptr ? error : "malformed");
		return;
	}
	if (is_builtin(lang_)) {
		prune(nullptr);
		succeed(LanguagePackState::builtin, "English is built in; release %s offers %u language pack(s)", release_,
			static_cast<unsigned>(offer_count_));
		return;
	}
	if (!found) {
		prune(nullptr);
		succeed(LanguagePackState::not_offered, "release %s offers no pack for '%s'", release_, lang_);
		return;
	}
	char name[BlobStore::kMaxNameLength + 1];
	blob_name(lang_, name, sizeof(name));
	char stored[65];
	if (stored_sha256(name, stored) && std::strcmp(stored, wanted_.sha256) == 0) {
		prune(name);
		succeed(LanguagePackState::installed, "pack '%s' of release %s is up to date", lang_, release_);
		return;
	}
	pack_due_ = true;
	arm(time_.monotonic_ms());
}

void LanguagePacks::pack_arrived() {
	if (writer_ == nullptr) {
		fail("pack %s: no body arrived", url_); // fetch_begin() was not called
		return;
	}
	if (received_ != wanted_.size) {
		drop_transfer();
		fail("pack %s refused: %u bytes, the manifest says %u", url_, static_cast<unsigned>(received_),
			static_cast<unsigned>(wanted_.size));
		return;
	}
	uint8_t digest[Sha256::kDigestSize];
	sha_.finish(digest);
	char hex[65];
	to_hex(digest, sizeof(digest), hex);
	if (std::strcmp(hex, wanted_.sha256) != 0) {
		drop_transfer();
		fail("pack %s refused: its SHA-256 %.16s... does not match the manifest's %.16s...", url_, hex, wanted_.sha256);
		return;
	}
	char name[BlobStore::kMaxNameLength + 1];
	blob_name(lang_, name, sizeof(name));
	size_t length = received_;
	BlobStatus status = writer_->commit();
	writer_ = nullptr;
	if (status != BlobStatus::ok) {
		fail("cannot store the pack %s: %s", name, blob_status_name(status));
		return;
	}
	prune(name);
	succeed(LanguagePackState::installed, "pack '%s' of release %s installed (%u bytes)", lang_, release_,
		static_cast<unsigned>(length));
}

bool LanguagePacks::stored_sha256(const char* name, char (&hex)[65]) const {
	size_t size = 0;
	if (store_.size(name, size) != BlobStatus::ok) {
		return false;
	}
	Sha256 sha;
	uint8_t buffer[256];
	size_t offset = 0;
	while (offset < size) {
		size_t length = 0;
		if (store_.read(name, offset, buffer, sizeof(buffer), length) != BlobStatus::ok || length == 0) {
			return false;
		}
		sha.update(buffer, length);
		offset += length;
	}
	uint8_t digest[Sha256::kDigestSize];
	sha.finish(digest);
	to_hex(digest, sizeof(digest), hex);
	return true;
}

void LanguagePacks::prune(const char* keep) {
	for (;;) {
		PackCollector collector(keep);
		store_.visit(collector);
		size_t removed = 0;
		for (size_t i = 0; i < collector.count(); ++i) {
			BlobStatus status = store_.remove(collector.name(i));
			if (status == BlobStatus::ok) {
				Log::info(kTag, "removed the language pack %s", collector.name(i));
				++removed;
			} else {
				Log::error(kTag, "cannot remove the language pack %s: %s", collector.name(i), blob_status_name(status));
			}
		}
		if (removed == 0 || collector.count() < PackCollector::kBatch) {
			return;
		}
	}
}

void LanguagePacks::succeed(LanguagePackState state, const char* fmt, ...) {
	backoff_.reset();
	pending_ = false;
	state_ = state;
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(detail_, sizeof(detail_), fmt, args);
	va_end(args);
	Log::info(kTag, "%s", detail_);
}

void LanguagePacks::fail(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(detail_, sizeof(detail_), fmt, args);
	va_end(args);
	state_ = LanguagePackState::failed;
	pending_ = true;
	uint32_t delay = backoff_.next();
	Log::warn(kTag, "language pack sync for '%s' failed: %s (retry in %u s)", lang_, detail_,
		static_cast<unsigned>(delay / 1000));
	arm(time_.monotonic_ms() + delay);
}

void LanguagePacks::write_json(JsonWriter& json) const {
	json.member("lang", lang_)
		.member("state", language_pack_state_name(state_))
		.member("detail", detail_)
		.member("release", release_);
	json.key("offered").begin_array();
	for (size_t i = 0; i < offer_count_; ++i) {
		json.begin_object().member("code", offers_[i].code).member("name", offers_[i].name).end_object();
	}
	json.end_array();
}

} // namespace hc
