/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/websocket.h"

#include <cstring>

#include "hc/sha.h"

namespace hc {
namespace websocket {

namespace {

constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64(const uint8_t* data, size_t length, char* out) {
	size_t n = 0;
	for (size_t i = 0; i < length; i += 3) {
		uint32_t v = uint32_t(data[i]) << 16;
		if (i + 1 < length) {
			v |= uint32_t(data[i + 1]) << 8;
		}
		if (i + 2 < length) {
			v |= data[i + 2];
		}
		out[n++] = kBase64[(v >> 18) & 63];
		out[n++] = kBase64[(v >> 12) & 63];
		out[n++] = i + 1 < length ? kBase64[(v >> 6) & 63] : '=';
		out[n++] = i + 2 < length ? kBase64[v & 63] : '=';
	}
	out[n] = '\0';
}

bool base64_char(char c) {
	return c != '\0' && std::strchr(kBase64, c) != nullptr;
}

} // namespace

bool accept_key(const char* client_key, char out[kAcceptKeyLength + 1]) {
	out[0] = '\0';
	// 16 bytes in base64: 22 characters and "==".
	if (client_key == nullptr || std::strlen(client_key) != 24 || client_key[22] != '=' || client_key[23] != '=') {
		return false;
	}
	for (int i = 0; i < 22; ++i) {
		if (!base64_char(client_key[i])) {
			return false;
		}
	}
	// The last character carries 2 bits; the 4 low bits of its value must be zero.
	const char* last = std::strchr(kBase64, client_key[21]);
	if (((last - kBase64) & 0x0F) != 0) {
		return false;
	}
	char joined[24 + sizeof(kGuid)];
	std::memcpy(joined, client_key, 24);
	std::memcpy(joined + 24, kGuid, sizeof(kGuid));
	uint8_t digest[20];
	sha1(joined, 24 + sizeof(kGuid) - 1, digest);
	base64(digest, sizeof(digest), out);
	return true;
}

size_t encode_header(uint8_t* out, Opcode opcode, bool fin, uint64_t payload_length, const uint8_t* mask) {
	size_t n = 0;
	out[n++] = static_cast<uint8_t>((fin ? 0x80 : 0) | static_cast<uint8_t>(opcode));
	uint8_t mask_bit = mask != nullptr ? 0x80 : 0;
	if (payload_length < 126) {
		out[n++] = static_cast<uint8_t>(mask_bit | payload_length);
	} else if (payload_length <= 0xFFFF) {
		out[n++] = static_cast<uint8_t>(mask_bit | 126);
		out[n++] = static_cast<uint8_t>(payload_length >> 8);
		out[n++] = static_cast<uint8_t>(payload_length);
	} else {
		out[n++] = static_cast<uint8_t>(mask_bit | 127);
		for (int i = 7; i >= 0; --i) {
			out[n++] = static_cast<uint8_t>(payload_length >> (8 * i));
		}
	}
	if (mask != nullptr) {
		std::memcpy(out + n, mask, 4);
		n += 4;
	}
	return n;
}

void apply_mask(uint8_t* data, size_t length, const uint8_t mask[4], size_t offset) {
	for (size_t i = 0; i < length; ++i) {
		data[i] ^= mask[(offset + i) & 3];
	}
}

size_t encode_close(uint8_t* out, uint16_t code, const char* reason) {
	out[0] = static_cast<uint8_t>(code >> 8);
	out[1] = static_cast<uint8_t>(code);
	size_t length = reason != nullptr ? std::strlen(reason) : 0;
	if (length > kMaxControlPayload - 2) {
		length = kMaxControlPayload - 2;
		// Do not cut a UTF-8 sequence.
		while (length > 0 && (static_cast<uint8_t>(reason[length]) & 0xC0) == 0x80) {
			--length;
		}
	}
	if (length > 0) {
		std::memcpy(out + 2, reason, length);
	}
	return 2 + length;
}

bool utf8_valid(const uint8_t* p, size_t length) {
	size_t i = 0;
	while (i < length) {
		uint8_t c = p[i];
		size_t n;
		uint32_t min;
		uint32_t code;
		if (c < 0x80) {
			++i;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			n = 2;
			min = 0x80;
			code = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			n = 3;
			min = 0x800;
			code = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			n = 4;
			min = 0x10000;
			code = c & 0x07;
		} else {
			return false;
		}
		if (length - i < n) {
			return false;
		}
		for (size_t k = 1; k < n; ++k) {
			if ((p[i + k] & 0xC0) != 0x80) {
				return false;
			}
			code = (code << 6) | (p[i + k] & 0x3F);
		}
		if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		i += n;
	}
	return true;
}

FrameDecoder::FrameDecoder(uint8_t* buffer, size_t capacity, Role role)
	: buffer_(buffer), capacity_(capacity), masked_(role == Role::server) {}

FrameDecoder::Event FrameDecoder::fail(uint16_t code, const char* why) {
	failed_ = true;
	close_code_ = code;
	error_ = why;
	return Event::error;
}

FrameDecoder::Event FrameDecoder::header_complete() {
	uint8_t b0 = header_[0];
	uint8_t b1 = header_[1];
	if ((b0 & 0x70) != 0) {
		return fail(kCloseProtocolError, "reserved bits set (no extension was negotiated)");
	}
	frame_fin_ = (b0 & 0x80) != 0;
	frame_opcode_ = static_cast<Opcode>(b0 & 0x0F);
	bool control = (b0 & 0x08) != 0;
	switch (frame_opcode_) {
	case Opcode::continuation:
	case Opcode::text:
	case Opcode::binary:
	case Opcode::close:
	case Opcode::ping:
	case Opcode::pong:
		break;
	default:
		return fail(kCloseProtocolError, "unknown opcode");
	}
	if (masked_ && (b1 & 0x80) == 0) {
		return fail(kCloseProtocolError, "client frame not masked");
	}
	if (!masked_ && (b1 & 0x80) != 0) {
		return fail(kCloseProtocolError, "server frame masked");
	}
	size_t length_bytes = 0;
	uint8_t short_length = b1 & 0x7F;
	if (short_length == 126) {
		length_bytes = 2;
	} else if (short_length == 127) {
		length_bytes = 8;
	}
	size_t need = 2 + length_bytes + (masked_ ? 4 : 0);
	if (header_have_ < need) {
		header_need_ = need;
		return Event::need_more; // read the rest of the header
	}
	uint64_t length = short_length;
	if (length_bytes > 0) {
		length = 0;
		for (size_t i = 0; i < length_bytes; ++i) {
			length = (length << 8) | header_[2 + i];
		}
		if ((length_bytes == 2 && length < 126) || (length_bytes == 8 && length <= 0xFFFF) ||
			(length >> 63) != 0) {
			return fail(kCloseProtocolError, "frame length not minimally encoded");
		}
	}
	if (masked_) {
		std::memcpy(mask_, header_ + 2 + length_bytes, 4);
	}
	if (control) {
		if (!frame_fin_) {
			return fail(kCloseProtocolError, "fragmented control frame");
		}
		if (length > kMaxControlPayload) {
			return fail(kCloseProtocolError, "control frame longer than 125 bytes");
		}
	} else if (frame_opcode_ == Opcode::continuation) {
		if (!in_message_) {
			return fail(kCloseProtocolError, "continuation frame without a message");
		}
	} else {
		if (in_message_) {
			return fail(kCloseProtocolError, "new message inside a fragmented message");
		}
		in_message_ = true;
		message_opcode_ = frame_opcode_;
		message_length_ = 0;
	}
	if (!control && length > capacity_ - message_length_) {
		return fail(kCloseTooBig, "message too long");
	}
	frame_length_ = length;
	frame_have_ = 0;
	in_payload_ = true;
	return Event::need_more;
}

FrameDecoder::Event FrameDecoder::frame_complete() {
	in_payload_ = false;
	header_have_ = 0;
	header_need_ = 2;
	bool control = (static_cast<uint8_t>(frame_opcode_) & 0x08) != 0;
	if (control) {
		if (frame_opcode_ == Opcode::close && frame_length_ == 1) {
			return fail(kCloseProtocolError, "close frame with a 1-byte payload");
		}
		if (frame_opcode_ == Opcode::close && frame_length_ > 2 && !utf8_valid(control_ + 2, frame_length_ - 2)) {
			return fail(kCloseInvalidPayload, "close reason is not UTF-8");
		}
		event_opcode_ = frame_opcode_;
		event_data_ = control_;
		event_length_ = static_cast<size_t>(frame_length_);
		return Event::control;
	}
	message_length_ += static_cast<size_t>(frame_length_);
	if (!frame_fin_) {
		return Event::need_more;
	}
	in_message_ = false;
	if (message_opcode_ == Opcode::text && !utf8_valid(buffer_, message_length_)) {
		return fail(kCloseInvalidPayload, "text message is not UTF-8");
	}
	event_opcode_ = message_opcode_;
	event_data_ = buffer_;
	event_length_ = message_length_;
	return Event::message;
}

FrameDecoder::Event FrameDecoder::push(const uint8_t* data, size_t length, size_t& consumed) {
	consumed = 0;
	if (failed_) {
		return Event::error;
	}
	for (;;) {
		if (!in_payload_) {
			while (header_have_ < header_need_ && consumed < length) {
				header_[header_have_++] = data[consumed++];
			}
			if (header_have_ < header_need_) {
				return Event::need_more;
			}
			Event event = header_complete();
			if (event == Event::error) {
				return event;
			}
			if (!in_payload_) {
				continue; // the extended header follows
			}
		}
		uint8_t* target = (static_cast<uint8_t>(frame_opcode_) & 0x08) != 0 ? control_ : buffer_ + message_length_;
		size_t want = static_cast<size_t>(frame_length_ - frame_have_);
		size_t take = length - consumed < want ? length - consumed : want;
		if (take > 0) {
			std::memcpy(target + frame_have_, data + consumed, take);
			if (masked_) {
				apply_mask(target + frame_have_, take, mask_, static_cast<size_t>(frame_have_));
			}
			frame_have_ += take;
			consumed += take;
		}
		if (frame_have_ < frame_length_) {
			return Event::need_more;
		}
		Event event = frame_complete();
		if (event != Event::need_more) {
			return event;
		}
		if (consumed == length) {
			return Event::need_more;
		}
	}
}

} // namespace websocket
} // namespace hc
