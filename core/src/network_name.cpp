/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/network_name.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.name";

void copy(char* out, size_t size, const char* text) {
	std::snprintf(out, size, "%s", text);
}

} // namespace

NetworkName::NetworkName(EventLoop& loop, const Clock& clock, Settings& settings, NameService& service)
	: loop_(loop), clock_(clock), settings_(settings), service_(service) {
	copy(name_, sizeof(name_), setting::kDefaultNetHostname);
}

NetworkName::~NetworkName() {
	stop_polling();
	loop_.cancel(settle_timer_);
	if (lookup_ != Lookup::none) {
		service_.cancel_lookup();
	}
}

bool NetworkName::start(Connectivity& connectivity) {
	stored_ = read_network_name(settings_, name_, kTag);
	if (stored_) {
		Log::info(kTag, "network name %s (stored)", name_);
	} else {
		copy(name_, sizeof(name_), setting::kDefaultNetHostname);
		Log::info(kTag, "no network name stored: one is picked when the network first comes up (%s unless taken)",
			name_);
	}
	connectivity_ = &connectivity;
	if (!connectivity.add_listener(this)) {
		Log::error(kTag, "cannot follow the network: the network name is not announced");
		return false;
	}
	return true;
}

const char* NetworkName::announced() const {
	const char* announced = applied_ ? service_.announced_name() : "";
	return announced != nullptr && announced[0] != '\0' ? announced : name_;
}

void NetworkName::connectivity_changed(const Connectivity& connectivity, LinkState) {
	if (!connectivity.is_up() || applied_ || lookup_ != Lookup::none || settling_) {
		return;
	}
	if (stored_) {
		apply();
		return;
	}
	set_candidate(1);
	start_pick_lookup();
}

// ---- The first pick ---------------------------------------------------------------------------

void NetworkName::set_candidate(unsigned index) {
	candidate_ = index;
	if (index <= 1) {
		copy(name_, sizeof(name_), setting::kDefaultNetHostname);
	} else {
		// The suffix the mDNS responder's own rename uses (name-2, name-3, ...).
		std::snprintf(name_, sizeof(name_), "%s-%u", setting::kDefaultNetHostname, index);
	}
}

void NetworkName::start_pick_lookup() {
	const char* reason = "";
	if (start_lookup(name_, Lookup::pick, &reason)) {
		return;
	}
	Log::warn(kTag, "cannot ask whether %s is free (%s): announcing it, mDNS probing decides", name_, reason);
	apply();
}

bool NetworkName::start_lookup(const char* name, Lookup kind, const char** reason) {
	const char* own = connectivity_ != nullptr ? connectivity_->ip() : "";
	if (!service_.start_lookup(name, own, kLookupMs, reason)) {
		return false;
	}
	lookup_ = kind;
	lookup_deadline_ = clock_.monotonic_ms() + kLookupMs + kLookupGraceMs;
	loop_.cancel(poll_timer_);
	poll_timer_ = loop_.schedule_periodic(clock_.monotonic_ms() + kPollMs, kPollMs,
		Callback::member<NetworkName, &NetworkName::poll>(this, "net_name.poll"));
	return true;
}

void NetworkName::stop_polling() {
	loop_.cancel(poll_timer_);
	poll_timer_ = TimerHandle();
}

void NetworkName::poll() {
	if (lookup_ == Lookup::none) {
		stop_polling();
		return;
	}
	NameLookup result = service_.poll_lookup();
	if (result == NameLookup::running) {
		if (clock_.monotonic_ms() < lookup_deadline_) {
			return;
		}
		service_.cancel_lookup();
		Log::warn(kTag, "the lookup of %s had no outcome after %u ms: given up",
			lookup_ == Lookup::pick ? name_ : target_, static_cast<unsigned>(kLookupMs + kLookupGraceMs));
		result = NameLookup::failed;
	}
	stop_polling();
	Lookup kind = lookup_;
	lookup_ = Lookup::none;
	if (kind == Lookup::pick) {
		pick_result(result);
	} else {
		rename_result(result);
	}
}

void NetworkName::pick_result(NameLookup result) {
	if (result == NameLookup::taken) {
		if (candidate_ < kMaxCandidates) {
			Log::info(kTag, "%s.local is taken on this network: trying the next name", name_);
			set_candidate(candidate_ + 1);
			start_pick_lookup();
			return;
		}
		Log::warn(kTag, "%s.local and the names before it are taken on this network: announcing it, mDNS probing "
			"renames it", name_);
	} else if (result == NameLookup::failed) {
		Log::warn(kTag, "the lookup of %s failed: announcing it, mDNS probing decides", name_);
	}
	apply();
}

void NetworkName::apply() {
	const char* reason = "";
	if (!service_.apply_name(name_, &reason)) {
		Log::error(kTag, "network name %s not announced: %s (retried at the next network up)", name_, reason);
		if (!stored_) {
			set_candidate(1); // the pick starts over
		}
		return;
	}
	applied_ = true;
	settling_ = true;
	loop_.cancel(settle_timer_);
	settle_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kSettleMs,
		Callback::member<NetworkName, &NetworkName::settle_due>(this, "net_name.settle_due"));
	Log::info(kTag, "network name %s.local announced (mDNS; DHCP host name with the next request)", name_);
}

void NetworkName::settle_due() {
	settle_timer_ = TimerHandle();
	settling_ = false;
	const char* announced = this->announced();
	if (stored_) {
		if (std::strcmp(announced, name_) != 0) {
			Log::warn(kTag, "%s.local is taken on this network: announcing %s.local until the restart (%s stays %s)",
				name_, announced, setting::kNetHostname, name_);
		}
		return;
	}
	// The pick: the name the responder ended up with (probing may have renamed the candidate).
	char picked[setting::kMaxNetHostnameLength + 1];
	copy(picked, sizeof(picked), network_name_valid(announced) ? announced : name_);
	const char* error = nullptr;
	if (!store_network_name(settings_, picked, &error)) {
		Log::error(kTag, "network name %s not stored (%s): picked again at the next start", picked, error);
		return;
	}
	copy(name_, sizeof(name_), picked);
	stored_ = true;
	Log::info(kTag, "network name %s picked and stored (%s)", name_, setting::kNetHostname);
}

// ---- The user's change ------------------------------------------------------------------------

NetworkName::Begin NetworkName::begin_rename(const char* input, const char** code, const char** error) {
	*code = "settings.invalid";
	char target[setting::kMaxNetHostnameLength + 1];
	if (!normalize_network_name(input, target, error)) {
		return Begin::refused;
	}
	*code = "settings.busy";
	if (rename_ != Rename::idle) {
		*error = "another name is being checked: try again in a moment";
		return Begin::refused;
	}
	if (picking()) {
		*error = "the controller is still choosing its network name: try again in a few seconds";
		return Begin::refused;
	}
	copy(target_, sizeof(target_), target);
	if (std::strcmp(target_, name_) == 0 || std::strcmp(target_, announced()) == 0) {
		// Its own name: a lookup would find the controller itself answering.
		if (!commit_rename(error)) {
			*code = "settings.storage_error";
			return Begin::refused;
		}
		return Begin::done;
	}
	const char* reason = "";
	if (!start_lookup(target_, Lookup::rename, &reason)) {
		*code = "settings.name_unchecked";
		std::snprintf(rename_text_, sizeof(rename_text_), "cannot check whether the name is free: %s", reason);
		*error = rename_text_;
		Log::warn(kTag, "network name %s refused: %s", target_, rename_text_);
		return Begin::refused;
	}
	rename_ = Rename::checking;
	Log::info(kTag, "network name %s: asking the network whether another host uses it", target_);
	return Begin::checking;
}

void NetworkName::rename_result(NameLookup result) {
	if (result == NameLookup::taken) {
		std::snprintf(rename_text_, sizeof(rename_text_), "%s is already in use on this network", target_);
		Log::warn(kTag, "network name %s refused: %s", target_, rename_text_);
		finish_rename(false, "settings.name_taken");
		return;
	}
	if (result != NameLookup::free) {
		copy(rename_text_, sizeof(rename_text_), "cannot check whether the name is free: the lookup failed");
		Log::warn(kTag, "network name %s refused: %s", target_, rename_text_);
		finish_rename(false, "settings.name_unchecked");
		return;
	}
	const char* error = nullptr;
	if (!commit_rename(&error)) {
		copy(rename_text_, sizeof(rename_text_), error);
		finish_rename(false, "settings.storage_error");
		return;
	}
	finish_rename(true, "");
}

void NetworkName::finish_rename(bool ok, const char* code) {
	rename_ = Rename::finished;
	rename_ok_ = ok;
	rename_code_ = code;
	if (rename_listener_) {
		rename_listener_();
	}
}

bool NetworkName::commit_rename(const char** error) {
	if (!store_network_name(settings_, target_, error)) {
		Log::error(kTag, "network name %s not stored: %s", target_, *error);
		return false;
	}
	char previous[setting::kMaxNetHostnameLength + 1];
	copy(previous, sizeof(previous), announced());
	copy(name_, sizeof(name_), target_);
	stored_ = true;
	Log::info(kTag, "network name changed to %s (was %s)", name_, previous);
	if (applied_) {
		apply(); // at once; before the network first came up it is announced then
	}
	return true;
}

bool NetworkName::rename_finished(bool& ok, const char** code, const char** error) {
	if (rename_ == Rename::checking) {
		return false;
	}
	if (rename_ == Rename::idle) {
		ok = false;
		*code = "api.not_deferred";
		*error = "no answer is pending";
		return true;
	}
	rename_ = Rename::idle;
	ok = rename_ok_;
	*code = rename_code_;
	*error = rename_text_;
	return true;
}

} // namespace hc
