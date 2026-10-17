/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/improv_protocol.h"

#include <cstring>

namespace hc {

namespace improv {

namespace {

constexpr uint8_t kMagic[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

} // namespace

const char* state_name(State state) {
	switch (state) {
	case State::authorization_required:
		return "authorization required";
	case State::authorized:
		return "authorized";
	case State::provisioning:
		return "provisioning";
	case State::provisioned:
		return "provisioned";
	}
	return "?";
}

uint8_t checksum(const uint8_t* bytes, size_t length) {
	uint8_t sum = 0;
	for (size_t i = 0; i < length; ++i) {
		sum = static_cast<uint8_t>(sum + bytes[i]);
	}
	return sum;
}

size_t build_frame(uint8_t type, const uint8_t* data, size_t data_length, uint8_t* out, size_t capacity) {
	size_t total = kHeaderSize + data_length + 1;
	if (data_length > 255 || capacity < total) {
		return 0;
	}
	std::memcpy(out, kMagic, sizeof(kMagic));
	out[6] = kVersion;
	out[7] = type;
	out[8] = static_cast<uint8_t>(data_length);
	if (data_length > 0) {
		std::memcpy(out + kHeaderSize, data, data_length);
	}
	out[total - 1] = checksum(out, total - 1);
	return total;
}

size_t build_current_state(State state, uint8_t* out, size_t capacity) {
	uint8_t value = static_cast<uint8_t>(state);
	return build_frame(kTypeCurrentState, &value, 1, out, capacity);
}

size_t build_error(Error error, uint8_t* out, size_t capacity) {
	uint8_t value = static_cast<uint8_t>(error);
	return build_frame(kTypeErrorState, &value, 1, out, capacity);
}

size_t build_rpc_result(uint8_t command, const char* const* strings, size_t count, uint8_t* out, size_t capacity) {
	uint8_t data[255];
	size_t n = 0;
	data[n++] = command;
	data[n++] = 0; // total length of the strings, patched below
	for (size_t i = 0; i < count; ++i) {
		size_t length = std::strlen(strings[i]);
		if (length > 255 || n + 1 + length > sizeof(data)) {
			return 0;
		}
		data[n++] = static_cast<uint8_t>(length);
		std::memcpy(data + n, strings[i], length);
		n += length;
	}
	data[1] = static_cast<uint8_t>(n - 2); // n <= 255, so this fits
	return build_frame(kTypeRpcResult, data, n, out, capacity);
}

bool parse_wifi_settings(const uint8_t* payload, size_t length, char* ssid, size_t ssid_capacity, char* psk,
	size_t psk_capacity) {
	if (length < 1) {
		return false;
	}
	size_t ssid_length = payload[0];
	if (ssid_length == 0 || length < 1 + ssid_length + 1) {
		return false;
	}
	size_t psk_length = payload[1 + ssid_length];
	if (length != 1 + ssid_length + 1 + psk_length) {
		return false;
	}
	if (ssid_length + 1 > ssid_capacity || psk_length + 1 > psk_capacity) {
		return false;
	}
	std::memcpy(ssid, payload + 1, ssid_length);
	ssid[ssid_length] = '\0';
	std::memcpy(psk, payload + 1 + ssid_length + 1, psk_length);
	psk[psk_length] = '\0';
	return true;
}

} // namespace improv

// ---- ImprovReader -----------------------------------------------------------------------------

void ImprovReader::add_console(uint8_t byte) {
	console_[console_length_++] = byte;
}

ImprovReader::Event ImprovReader::release_and_restart(uint8_t byte) {
	// Only magic bytes (and at most the version byte) are ever released: everything after the
	// version is binary and never reaches the console.
	for (size_t i = 0; i < length_; ++i) {
		add_console(frame_[i]);
	}
	length_ = 0;
	expected_ = 0;
	if (byte == improv::kMagic[0]) {
		frame_[length_++] = byte;
	} else {
		add_console(byte);
	}
	return console_length_ > 0 ? Event::console : Event::none;
}

ImprovReader::Event ImprovReader::feed(uint8_t byte, uint64_t now_ms) {
	console_length_ = 0;
	last_byte_ms_ = now_ms;
	if (length_ < sizeof(improv::kMagic)) {
		if (byte != improv::kMagic[length_]) {
			return release_and_restart(byte);
		}
		frame_[length_++] = byte;
		return Event::none;
	}
	if (length_ == sizeof(improv::kMagic) && byte != improv::kVersion) {
		// "IMPROV" followed by something else: text that happens to contain the magic.
		return release_and_restart(byte);
	}
	frame_[length_++] = byte;
	if (length_ == improv::kHeaderSize) {
		expected_ = improv::kHeaderSize + frame_[8] + 1;
		return Event::none;
	}
	if (length_ < improv::kHeaderSize || length_ < expected_) {
		return Event::none;
	}
	bool valid = improv::checksum(frame_, expected_ - 1) == frame_[expected_ - 1];
	length_ = 0;
	expected_ = 0;
	if (!valid) {
		bad_reason_ = "checksum mismatch";
		return Event::bad_frame;
	}
	return Event::frame;
}

ImprovReader::Event ImprovReader::poll(uint64_t now_ms) {
	console_length_ = 0;
	if (length_ == 0 || now_ms - last_byte_ms_ < kGapMs) {
		return Event::none;
	}
	return flush();
}

ImprovReader::Event ImprovReader::flush() {
	console_length_ = 0;
	if (length_ == 0) {
		return Event::none;
	}
	if (length_ > sizeof(improv::kMagic)) {
		length_ = 0;
		expected_ = 0;
		bad_reason_ = "incomplete frame";
		return Event::bad_frame;
	}
	for (size_t i = 0; i < length_; ++i) {
		add_console(frame_[i]);
	}
	length_ = 0;
	return Event::console;
}

} // namespace hc
