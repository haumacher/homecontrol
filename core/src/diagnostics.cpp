/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/diagnostics.h"

#include "hc/log.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>

namespace hc {

const char* reset_reason_name(ResetReason reason) {
	switch (reason) {
	case ResetReason::unknown: return "unknown";
	case ResetReason::power_on: return "power-on";
	case ResetReason::external: return "external reset";
	case ResetReason::software: return "software restart";
	case ResetReason::panic: return "panic";
	case ResetReason::interrupt_watchdog: return "interrupt watchdog";
	case ResetReason::task_watchdog: return "task watchdog";
	case ResetReason::other_watchdog: return "other watchdog";
	case ResetReason::deep_sleep: return "deep sleep wake-up";
	case ResetReason::brownout: return "brownout";
	case ResetReason::sdio: return "SDIO reset";
	case ResetReason::usb: return "USB reset";
	case ResetReason::jtag: return "JTAG reset";
	case ResetReason::efuse: return "eFuse error";
	case ResetReason::power_glitch: return "power glitch";
	case ResetReason::cpu_lockup: return "CPU lock-up";
	}
	return "unknown";
}

bool reset_reason_is_crash(ResetReason reason) {
	switch (reason) {
	case ResetReason::panic:
	case ResetReason::interrupt_watchdog:
	case ResetReason::task_watchdog:
	case ResetReason::other_watchdog:
	case ResetReason::cpu_lockup:
		return true;
	default:
		return false;
	}
}

namespace {

class JsonFacts : public DiagnosticsVisitor {
public:
	explicit JsonFacts(JsonWriter& json) : json_(json) {}

	void number(const char* key, uint64_t value) override { json_.member(key, value); }
	void text(const char* key, const char* value) override { json_.member(key, value); }
	void flag(const char* key, bool value) override { json_.member(key, value); }

private:
	JsonWriter& json_;
};

class TextFacts : public DiagnosticsVisitor {
public:
	TextFacts(char* buffer, size_t size) : buffer_(buffer), size_(size) {
		if (size_ > 0) {
			buffer_[0] = '\0';
		}
	}

	void number(const char* key, uint64_t value) override {
		append("%s%s %" PRIu64, separator(), key, value);
	}
	void text(const char* key, const char* value) override { append("%s%s %s", separator(), key, value); }
	void flag(const char* key, bool value) override { append("%s%s %s", separator(), key, value ? "yes" : "no"); }

	size_t length() const { return length_; }

private:
	const char* separator() const { return length_ == 0 ? "" : ", "; }

	void append(const char* format, ...) HC_PRINTF_FORMAT(2, 3) {
		if (length_ + 1 >= size_) {
			return;
		}
		va_list args;
		va_start(args, format);
		int n = std::vsnprintf(buffer_ + length_, size_ - length_, format, args);
		va_end(args);
		if (n < 0) {
			return;
		}
		length_ += static_cast<size_t>(n);
		if (length_ >= size_) {
			length_ = size_ - 1; // truncated
		}
	}

	char* buffer_;
	size_t size_;
	size_t length_ = 0;
};

} // namespace

void write_boot_status(JsonWriter& json, const Diagnostics* diagnostics) {
	json.key("boot").begin_object();
	if (diagnostics == nullptr) {
		json.member("reason", reset_reason_name(ResetReason::unknown));
	} else {
		json.member("reason", reset_reason_name(diagnostics->reset_reason()));
		JsonFacts facts(json);
		diagnostics->visit_boot(facts);
	}
	json.end_object();
}

size_t format_boot_facts(const Diagnostics& diagnostics, char* buffer, size_t size) {
	TextFacts facts(buffer, size);
	diagnostics.visit_boot(facts);
	return facts.length();
}

} // namespace hc
