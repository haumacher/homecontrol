/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/connectivity.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.net";

/// "4 s" for whole seconds, "1500 ms" otherwise.
void format_delay(char* buffer, size_t size, uint64_t ms) {
	if (ms % 1000 == 0) {
		std::snprintf(buffer, size, "%llu s", static_cast<unsigned long long>(ms / 1000));
	} else {
		std::snprintf(buffer, size, "%llu ms", static_cast<unsigned long long>(ms));
	}
}

} // namespace

const char* link_state_name(LinkState state) {
	switch (state) {
	case LinkState::down:
		return "down";
	case LinkState::connecting:
		return "connecting";
	case LinkState::up:
		return "up";
	}
	return "?";
}

const char* network_mode_name(NetworkMode mode) {
	switch (mode) {
	case NetworkMode::normal:
		return "normal";
	case NetworkMode::unprovisioned:
		return "unprovisioned";
	case NetworkMode::repairing:
		return "re-pairing";
	case NetworkMode::failsafe_pairing:
		return "failsafe (pairing)";
	case NetworkMode::failsafe_connecting:
		return "failsafe (stored credentials)";
	}
	return "?";
}

Connectivity::Connectivity(EventLoop& loop, const Clock& clock, NetworkDriver& driver)
	: loop_(loop), clock_(clock), driver_(driver) {}

Connectivity::~Connectivity() {
	cancel_action();
	cancel_timeout();
	cancel_phase();
	cancel_pause_timer();
}

// ---- Requests ---------------------------------------------------------------------------------

void Connectivity::start() {
	if (started_) {
		Log::error(kTag, "refusing start: connectivity already started");
		return;
	}
	started_ = true;
	evaluate(0);
}

void Connectivity::request_connect(const char* reason) {
	if (refuse_reentry("request_connect")) {
		return;
	}
	Log::info(kTag, "network restart requested: %s", reason);
	started_ = true;
	cancel_phase();
	cancel_timeout();
	cancel_action();
	bool stopped = false;
	if (pairing_) {
		end_pairing();
		stopped = true;
	}
	failures_ = 0;
	backoff_.reset();
	evaluate(stopped ? kSettleMs : 0);
}

bool Connectivity::request_pairing(const char* trigger) {
	if (refuse_reentry("request_pairing")) {
		return false;
	}
	if (pairing_active()) {
		Log::warn(kTag, "refusing pairing (%s): pairing is already active", trigger);
		return false;
	}
	Log::info(kTag, "pairing requested (%s)", trigger);
	started_ = true;
	cancel_phase();
	begin_pairing(driver_.provisioned() ? NetworkMode::repairing : NetworkMode::unprovisioned, 0);
	return true;
}

bool Connectivity::pause_pairing(const char* why) {
	if (refuse_reentry("pause_pairing")) {
		return false;
	}
	if (paused_) {
		Log::warn(kTag, "refusing pause for %s: already paused for %s", why, pause_reason_);
		return false;
	}
	paused_ = true;
	std::snprintf(pause_reason_, sizeof(pause_reason_), "%s", why);
	uint64_t now = clock_.monotonic_ms();
	station_free_at_ = now;
	pause_timer_ = loop_.schedule_at(now + kMaxPauseMs, Callback::member<Connectivity, &Connectivity::pause_due>(this));
	if (pairing_) {
		cancel_timeout();
		end_pairing();
		held_ = Action::start_pairing;
		station_free_at_ = now + kSettleMs;
		Log::info(kTag, "network paused for %s: pairing stopped, resumes afterwards", why);
	} else if (state_ == LinkState::connecting) {
		cancel_timeout();
		driver_disconnect();
		held_ = Action::connect;
		station_free_at_ = now + kSettleMs;
		Log::info(kTag, "network paused for %s: attempt aborted, repeated afterwards", why);
		char reason[kMaxReason];
		std::snprintf(reason, sizeof(reason), "paused for %s", why);
		set_state(LinkState::down, reason);
	} else {
		Log::info(kTag, "network paused for %s", why);
	}
	return true;
}

void Connectivity::resume_pairing(const char* why) {
	if (refuse_reentry("resume_pairing")) {
		return;
	}
	if (!paused_) {
		Log::warn(kTag, "resume (%s) while not paused, ignored", why);
		return;
	}
	paused_ = false;
	cancel_pause_timer();
	Action held = held_;
	held_ = Action::none;
	if (held != Action::none && action_ == Action::none) {
		Log::info(kTag, "network resumed (%s): %s", why, held == Action::connect ? "connecting" : "pairing");
		schedule(held, kSettleMs);
	} else {
		Log::info(kTag, "network resumed (%s)", why);
	}
}

void Connectivity::pause_due() {
	pause_timer_ = TimerHandle();
	Log::warn(kTag, "network pause for %s not ended after %u s: resuming", pause_reason_,
		static_cast<unsigned>(kMaxPauseMs / 1000));
	resume_pairing("pause timeout");
}

// ---- Reports from the driver ------------------------------------------------------------------

void Connectivity::link_connecting() {
	if (refuse_reentry("link_connecting")) {
		return;
	}
	if (state_ == LinkState::connecting) {
		Log::debug(kTag, "driver reports connecting, already connecting");
		return;
	}
	if (pairing_) {
		Log::debug(kTag, "driver reports connecting while pairing, ignored");
		return;
	}
	cancel_action();
	cancel_timeout();
	timeout_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kConnectTimeoutMs,
		Callback::member<Connectivity, &Connectivity::timeout_due>(this));
	set_state(LinkState::connecting, "driver started connecting");
}

void Connectivity::link_up(const char* ip) {
	if (refuse_reentry("link_up")) {
		return;
	}
	if (ip == nullptr || ip[0] == '\0' || std::strlen(ip) >= kMaxAddress) {
		Log::error(kTag, "refusing link_up: address missing or longer than %u characters",
			static_cast<unsigned>(kMaxAddress - 1));
		return;
	}
	if (state_ == LinkState::up && std::strcmp(ip_, ip) == 0) {
		Log::debug(kTag, "driver reports up with %s, already up", ip);
		return;
	}
	if (!pairing_) {
		cancel_timeout();
	}
	if (action_ == Action::connect || held_ == Action::connect) {
		cancel_action();
	}
	failures_ = 0;
	backoff_.reset();
	if (mode_ == NetworkMode::failsafe_pairing || mode_ == NetworkMode::failsafe_connecting) {
		exit_failsafe();
	} else if (mode_ == NetworkMode::unprovisioned) {
		if (pairing_) {
			cancel_timeout();
			end_pairing();
		}
		cancel_action();
		set_mode(NetworkMode::normal, "the link came up");
	}
	std::snprintf(ip_, sizeof(ip_), "%s", ip);
	set_state(LinkState::up, "link up");
}

void Connectivity::link_down(const char* reason) {
	if (refuse_reentry("link_down")) {
		return;
	}
	if (pairing_ || action_ != Action::none) {
		// Pairing left the access point, or the report belongs to an activity the core stopped.
		if (state_ != LinkState::down) {
			set_state(LinkState::down, reason);
		} else {
			Log::debug(kTag, "driver reports down (%s) while %s, ignored", reason,
				pairing_ ? "pairing" : "waiting for the next step");
		}
		return;
	}
	if (state_ == LinkState::down) {
		Log::debug(kTag, "driver reports down (%s), already down", reason);
		return;
	}
	fail(reason);
}

void Connectivity::pairing_succeeded(const char* detail) {
	if (refuse_reentry("pairing_succeeded")) {
		return;
	}
	if (!pairing_) {
		Log::debug(kTag, "pairing success reported while not pairing, ignored");
		return;
	}
	cancel_timeout();
	cancel_phase();
	end_pairing();
	failures_ = 0;
	backoff_.reset();
	Log::info(kTag, "pairing succeeded: %s", detail);
	set_mode(NetworkMode::normal, "new credentials");
	schedule(Action::connect, kSettleMs);
}

void Connectivity::pairing_failed(const char* reason) {
	if (refuse_reentry("pairing_failed")) {
		return;
	}
	if (!pairing_) {
		Log::debug(kTag, "pairing failure (%s) reported while not pairing, ignored", reason);
		return;
	}
	cancel_timeout();
	end_pairing();
	switch (mode_) {
	case NetworkMode::unprovisioned:
	case NetworkMode::failsafe_pairing: {
		char delay[16];
		format_delay(delay, sizeof(delay), kPairingRestartMs);
		Log::warn(kTag, "pairing failed: %s; listening again in %s", reason, delay);
		schedule(Action::start_pairing, kPairingRestartMs);
		break;
	}
	case NetworkMode::repairing:
		Log::warn(kTag, "pairing failed: %s; back to the stored credentials", reason);
		set_mode(NetworkMode::normal, "pairing failed");
		if (state_ != LinkState::up) {
			backoff_.reset();
			schedule(Action::connect, kSettleMs);
		}
		break;
	case NetworkMode::normal:
	case NetworkMode::failsafe_connecting:
		Log::warn(kTag, "pairing failed: %s", reason);
		break;
	}
}

bool Connectivity::add_listener(ConnectivityListener* listener) {
	if (listener == nullptr) {
		Log::error(kTag, "refusing null connectivity listener");
		return false;
	}
	if (listener_count_ == kListenerCapacity) {
		Log::error(kTag, "connectivity listeners exhausted (capacity %u), refusing listener",
			static_cast<unsigned>(kListenerCapacity));
		return false;
	}
	listeners_[listener_count_++] = listener;
	return true;
}

// ---- The workflow -----------------------------------------------------------------------------

void Connectivity::evaluate(uint32_t delay_ms) {
	if (driver_.provisioned()) {
		set_mode(NetworkMode::normal, "credentials stored");
		schedule(Action::connect, delay_ms);
	} else {
		Log::warn(kTag, "no credentials stored: pairing until a partner answers");
		begin_pairing(NetworkMode::unprovisioned, delay_ms);
	}
}

void Connectivity::attempt() {
	cancel_timeout();
	++attempts_;
	switch (driver_connect()) {
	case ConnectResult::started: {
		timeout_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kConnectTimeoutMs,
			Callback::member<Connectivity, &Connectivity::timeout_due>(this));
		char reason[kMaxReason];
		std::snprintf(reason, sizeof(reason), "attempt %u", static_cast<unsigned>(attempts_));
		set_state(LinkState::connecting, reason);
		break;
	}
	case ConnectResult::failed:
		fail("the driver could not start an attempt");
		break;
	case ConnectResult::refused:
		Log::warn(kTag, "the driver has no usable credentials: pairing until a partner answers");
		cancel_phase();
		begin_pairing(NetworkMode::unprovisioned, 0);
		break;
	}
}

void Connectivity::begin_pairing(NetworkMode mode, uint32_t delay_ms) {
	cancel_action();
	cancel_timeout();
	bool keep_link = mode == NetworkMode::repairing && state_ == LinkState::up;
	if (state_ != LinkState::down && !keep_link) {
		driver_disconnect();
		if (delay_ms < kSettleMs) {
			delay_ms = kSettleMs;
		}
		set_mode(mode, "pairing");
		schedule(Action::start_pairing, delay_ms);
		set_state(LinkState::down, "stopped for pairing");
		return;
	}
	set_mode(mode, "pairing");
	backoff_.reset();
	schedule(Action::start_pairing, delay_ms);
}

void Connectivity::fail(const char* reason) {
	cancel_timeout();
	if (failures_ < UINT32_MAX) {
		++failures_;
	}
	if (mode_ == NetworkMode::normal && failures_ >= kFailsafeFailures) {
		enter_failsafe();
		set_state(LinkState::down, reason);
		return;
	}
	schedule(Action::connect, backoff_.next());
	set_state(LinkState::down, reason);
}

void Connectivity::enter_failsafe() {
	char phase[16];
	format_delay(phase, sizeof(phase), kFailsafePhaseMs);
	Log::error(kTag,
		"stored credentials failed %u times in a row: entering the failsafe (alternating %s of pairing "
		"and of retrying them; they stay stored)",
		static_cast<unsigned>(failures_), phase);
	driver_disconnect();
	cancel_phase();
	phase_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kFailsafePhaseMs,
		Callback::member<Connectivity, &Connectivity::phase_due>(this));
	set_mode(NetworkMode::failsafe_pairing, "stored credentials failing");
	backoff_.reset();
	schedule(Action::start_pairing, kSettleMs);
}

void Connectivity::exit_failsafe() {
	cancel_phase();
	if (pairing_) {
		cancel_timeout();
		end_pairing();
	}
	if (action_ == Action::start_pairing || held_ == Action::start_pairing) {
		cancel_action();
	}
	set_mode(NetworkMode::normal, "failsafe over");
}

void Connectivity::phase_due() {
	phase_timer_ = TimerHandle();
	char phase[16];
	format_delay(phase, sizeof(phase), kFailsafePhaseMs);
	phase_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kFailsafePhaseMs,
		Callback::member<Connectivity, &Connectivity::phase_due>(this));
	cancel_timeout();
	cancel_action();
	if (mode_ == NetworkMode::failsafe_pairing) {
		Log::info(kTag, "failsafe: pairing phase over, retrying the stored credentials for %s", phase);
		if (pairing_) {
			end_pairing();
		}
		set_mode(NetworkMode::failsafe_connecting, "phase over");
		backoff_.reset();
		schedule(Action::connect, kSettleMs);
	} else if (mode_ == NetworkMode::failsafe_connecting) {
		Log::info(kTag, "failsafe: stored-credentials phase over, pairing for %s", phase);
		begin_pairing(NetworkMode::failsafe_pairing, kSettleMs);
	} else {
		cancel_phase();
	}
}

void Connectivity::end_pairing() {
	pairing_ = false;
	driver_stop_pairing();
}

void Connectivity::schedule(Action action, uint32_t delay_ms) {
	cancel_action(); // a newly decided step also supersedes a held one
	action_ = action;
	if (delay_ms == 0) {
		action_due();
		return;
	}
	action_at_ = clock_.monotonic_ms() + delay_ms;
	action_timer_ = loop_.schedule_at(action_at_, Callback::member<Connectivity, &Connectivity::action_due>(this));
	if (!action_timer_.valid()) {
		Log::error(kTag, "network: next step not scheduled (no timer)");
		action_ = Action::none;
		action_at_ = EventLoop::kNever;
	}
}

void Connectivity::action_due() {
	Action action = action_;
	action_ = Action::none;
	action_timer_ = TimerHandle();
	action_at_ = EventLoop::kNever;
	if (paused_ && action != Action::none) {
		held_ = action;
		Log::info(kTag, "network: %s held while paused for %s", action == Action::connect ? "connect" : "pairing",
			pause_reason_);
		return;
	}
	switch (action) {
	case Action::none:
		break;
	case Action::connect:
		attempt();
		break;
	case Action::start_pairing:
		if (driver_start_pairing()) {
			pairing_ = true;
			timeout_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kPairingTimeoutMs,
				Callback::member<Connectivity, &Connectivity::timeout_due>(this));
			Log::info(kTag, "network pairing (%s): press the pairing (WPS) button on the router",
				network_mode_name(mode_));
		} else {
			uint32_t delay = backoff_.next();
			char text[16];
			format_delay(text, sizeof(text), delay);
			Log::warn(kTag, "pairing could not start; trying again in %s", text);
			schedule(Action::start_pairing, delay);
		}
		break;
	}
}

void Connectivity::timeout_due() {
	timeout_timer_ = TimerHandle();
	char reason[kMaxReason];
	if (pairing_) {
		std::snprintf(reason, sizeof(reason), "no pairing outcome after %u s",
			static_cast<unsigned>(kPairingTimeoutMs / 1000));
		pairing_failed(reason);
	} else {
		std::snprintf(reason, sizeof(reason), "no link after %u s", static_cast<unsigned>(kConnectTimeoutMs / 1000));
		fail(reason);
	}
}

void Connectivity::cancel_action() {
	if (action_timer_.valid()) {
		loop_.cancel(action_timer_);
		action_timer_ = TimerHandle();
	}
	action_ = Action::none;
	held_ = Action::none;
	action_at_ = EventLoop::kNever;
}

void Connectivity::cancel_timeout() {
	if (timeout_timer_.valid()) {
		loop_.cancel(timeout_timer_);
		timeout_timer_ = TimerHandle();
	}
}

void Connectivity::cancel_pause_timer() {
	if (pause_timer_.valid()) {
		loop_.cancel(pause_timer_);
		pause_timer_ = TimerHandle();
	}
}

void Connectivity::cancel_phase() {
	if (phase_timer_.valid()) {
		loop_.cancel(phase_timer_);
		phase_timer_ = TimerHandle();
	}
}

// ---- Driver calls (guarded against synchronous reports) ---------------------------------------

ConnectResult Connectivity::driver_connect() {
	in_driver_ = true;
	ConnectResult result = driver_.connect();
	in_driver_ = false;
	return result;
}

void Connectivity::driver_disconnect() {
	in_driver_ = true;
	driver_.disconnect();
	in_driver_ = false;
}

bool Connectivity::driver_start_pairing() {
	in_driver_ = true;
	bool started = driver_.start_pairing();
	in_driver_ = false;
	return started;
}

void Connectivity::driver_stop_pairing() {
	in_driver_ = true;
	driver_.stop_pairing();
	in_driver_ = false;
}

bool Connectivity::refuse_reentry(const char* what) {
	if (in_driver_) {
		Log::error(kTag, "refusing %s from within a network driver call (report asynchronously)", what);
		return true;
	}
	if (notifying_) {
		Log::error(kTag, "refusing %s from a connectivity listener", what);
		return true;
	}
	return false;
}

// ---- State ------------------------------------------------------------------------------------

void Connectivity::set_mode(NetworkMode mode, const char* why) {
	if (mode_ == mode) {
		return;
	}
	Log::info(kTag, "network mode %s -> %s (%s)", network_mode_name(mode_), network_mode_name(mode), why);
	mode_ = mode;
}

void Connectivity::set_state(LinkState state, const char* reason) {
	LinkState previous = state_;
	state_ = state;
	std::snprintf(reason_, sizeof(reason_), "%s", reason == nullptr ? "" : reason);
	if (state != LinkState::up) {
		ip_[0] = '\0';
	}

	switch (state) {
	case LinkState::up:
		Log::info(kTag, "network up %s", ip_);
		break;
	case LinkState::connecting:
		Log::info(kTag, "network connecting (%s)", reason_);
		break;
	case LinkState::down: {
		char delay[16] = "";
		if (action_at_ != EventLoop::kNever) {
			uint64_t now = clock_.monotonic_ms();
			format_delay(delay, sizeof(delay), action_at_ > now ? action_at_ - now : 0);
		}
		if (action_ == Action::connect) {
			Log::warn(kTag, "network down: %s; reconnect in %s (failure %u)", reason_, delay,
				static_cast<unsigned>(failures_));
		} else if (action_ == Action::start_pairing) {
			Log::warn(kTag, "network down: %s; pairing in %s", reason_, delay);
		} else if (pairing_) {
			Log::warn(kTag, "network down: %s; pairing", reason_);
		} else if (paused_) {
			Log::info(kTag, "network down: %s; next step after the pause", reason_);
		} else {
			Log::error(kTag, "network down: %s; no next step scheduled", reason_);
		}
		break;
	}
	}

	if (previous == state && state != LinkState::up) {
		return;
	}
	notifying_ = true;
	for (size_t i = 0; i < listener_count_; ++i) {
		listeners_[i]->connectivity_changed(*this, previous);
	}
	notifying_ = false;
}

} // namespace hc
