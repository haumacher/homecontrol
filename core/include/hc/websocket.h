/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_WEBSOCKET_H
#define HC_WEBSOCKET_H

#include <cstddef>
#include <cstdint>

namespace hc {
namespace websocket {

/// The WebSocket protocol (RFC 6455) as a pure codec: the handshake key, frame headers, masking
/// and a server-side decoder that reassembles fragmented messages. The POSIX build speaks it over
/// a plain socket (doc/web.md); the ESP build uses the WebSocket support of esp_http_server.

enum class Opcode : uint8_t {
	continuation = 0x0,
	text = 0x1,
	binary = 0x2,
	close = 0x8,
	ping = 0x9,
	pong = 0xA,
};

/// Close codes used here (RFC 6455 section 7.4.1).
constexpr uint16_t kCloseNormal = 1000;
constexpr uint16_t kCloseGoingAway = 1001;
constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseUnsupported = 1003;
constexpr uint16_t kCloseNoStatus = 1005;
constexpr uint16_t kCloseInvalidPayload = 1007;
constexpr uint16_t kClosePolicy = 1008;
constexpr uint16_t kCloseTooBig = 1009;
constexpr uint16_t kCloseTryAgainLater = 1013;

/// Longest frame header: 2 + 8 (length) + 4 (mask).
constexpr size_t kMaxHeader = 14;

/// Longest control frame payload.
constexpr size_t kMaxControlPayload = 125;

/// Length of Sec-WebSocket-Accept (base64 of a SHA-1).
constexpr size_t kAcceptKeyLength = 28;

/// Computes Sec-WebSocket-Accept from the client's Sec-WebSocket-Key into `out` (29 bytes with the
/// NUL). False if the key is not the base64 encoding of 16 bytes (RFC 6455 section 4.2.1).
bool accept_key(const char* client_key, char out[kAcceptKeyLength + 1]);

/// Writes a frame header into `out` (kMaxHeader bytes); `mask` (4 bytes) or nullptr for a server
/// frame. Returns the header length.
size_t encode_header(uint8_t* out, Opcode opcode, bool fin, uint64_t payload_length, const uint8_t* mask = nullptr);

/// XORs `data` with the 4-byte mask, starting at mask position `offset`.
void apply_mask(uint8_t* data, size_t length, const uint8_t mask[4], size_t offset = 0);

/// The payload of a close frame: the code (big endian) and a UTF-8 reason, cut to fit 125 bytes.
size_t encode_close(uint8_t* out, uint16_t code, const char* reason);

/// Whether `data` is valid UTF-8.
bool utf8_valid(const uint8_t* data, size_t length);

/// Decodes what a client sends: frames must be masked (section 5.1), reserved bits zero, control
/// frames unfragmented and at most 125 bytes; data frames are reassembled into one message in the
/// caller's buffer (a text message is checked for valid UTF-8). The decoder stops at each complete
/// message or control frame (Event::message, Event::control); its payload stays valid until the
/// next push(). Any violation ends with Event::error, close_code() and error() say why; the
/// connection must then be closed with that code.
class FrameDecoder {
public:
	enum class Event : uint8_t {
		need_more, ///< All input consumed, nothing complete yet.
		message,   ///< A complete text or binary message: opcode(), data(), length().
		control,   ///< A ping, pong or close frame: opcode(), data(), length().
		error,     ///< Protocol violation: close_code(), error().
	};

	/// `buffer` receives the payload of data messages; a message longer than `capacity` is refused
	/// with kCloseTooBig.
	FrameDecoder(uint8_t* buffer, size_t capacity);

	/// Consumes bytes up to the next event; `consumed` tells how many. Call again with the rest.
	Event push(const uint8_t* data, size_t length, size_t& consumed);

	Opcode opcode() const { return event_opcode_; }
	const uint8_t* data() const { return event_data_; }
	size_t length() const { return event_length_; }

	uint16_t close_code() const { return close_code_; }
	const char* error() const { return error_; }

private:
	Event fail(uint16_t code, const char* why);
	Event header_complete();
	Event frame_complete();

	uint8_t* buffer_;
	size_t capacity_;

	uint8_t header_[kMaxHeader] = {};
	size_t header_have_ = 0;
	size_t header_need_ = 2;

	Opcode frame_opcode_ = Opcode::continuation;
	bool frame_fin_ = false;
	uint8_t mask_[4] = {};
	uint64_t frame_length_ = 0;
	uint64_t frame_have_ = 0;
	bool in_payload_ = false;

	bool in_message_ = false; ///< A fragmented data message is being assembled.
	Opcode message_opcode_ = Opcode::text;
	size_t message_length_ = 0;

	uint8_t control_[kMaxControlPayload] = {};

	Opcode event_opcode_ = Opcode::continuation;
	const uint8_t* event_data_ = nullptr;
	size_t event_length_ = 0;
	uint16_t close_code_ = 0;
	const char* error_ = "";
	bool failed_ = false;
};

} // namespace websocket
} // namespace hc

#endif // HC_WEBSOCKET_H
