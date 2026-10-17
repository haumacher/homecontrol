/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMPROV_PROTOCOL_H
#define HC_IMPROV_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The Improv Wi-Fi serial protocol, version 1 (https://www.improv-wifi.com/serial/), ported from
/// the phoneblock dongle's improv_proto.c. Every frame, in both directions:
///
///     "IMPROV" (6 bytes)  version (1)  type (1)  length (1)  data (length bytes)  checksum (1)
///
/// The checksum is the sum of all preceding bytes, the "IMPROV" magic included, modulo 256. This
/// layer is pure byte handling: no logging, no clock of its own, no shared state, so the ESP
/// platform runs ImprovReader in its serial reader task while hc::Improv runs on the event loop.
namespace improv {

constexpr uint8_t kVersion = 1;

/// Magic, version, type and length.
constexpr size_t kHeaderSize = 9;

/// Longest frame: header, 255 data bytes, checksum.
constexpr size_t kMaxFrame = kHeaderSize + 255 + 1;

/// Frame types.
constexpr uint8_t kTypeCurrentState = 0x01; ///< device -> client
constexpr uint8_t kTypeErrorState = 0x02;   ///< device -> client
constexpr uint8_t kTypeRpcCommand = 0x03;   ///< client -> device
constexpr uint8_t kTypeRpcResult = 0x04;    ///< device -> client

/// Current-state values.
enum class State : uint8_t {
	authorization_required = 0x01, ///< BLE only; never sent over serial
	authorized = 0x02,             ///< ready to accept credentials
	provisioning = 0x03,           ///< credentials received, connecting
	provisioned = 0x04,            ///< connected
};

/// Error-state values.
enum class Error : uint8_t {
	none = 0x00,
	invalid_rpc = 0x01,       ///< malformed frame or RPC (bad checksum, wrong lengths)
	unknown_rpc = 0x02,       ///< unknown RPC command
	unable_to_connect = 0x03, ///< the credentials did not lead to a connection
	not_authorized = 0x04,    ///< BLE only
	unknown = 0xFF,
};

/// RPC commands (first data byte of an RPC command frame).
constexpr uint8_t kRpcWifiSettings = 0x01;
constexpr uint8_t kRpcGetCurrentState = 0x02;
constexpr uint8_t kRpcGetDeviceInfo = 0x03;
constexpr uint8_t kRpcGetWifiNetworks = 0x04;

/// "authorized", "provisioning", ...
const char* state_name(State state);

/// Sum of `length` bytes modulo 256.
uint8_t checksum(const uint8_t* bytes, size_t length);

/// Writes a complete frame of `type` with `data` into `out`. Returns its size, or 0 if
/// `data_length` exceeds 255 or `capacity` is too small.
size_t build_frame(uint8_t type, const uint8_t* data, size_t data_length, uint8_t* out, size_t capacity);

size_t build_current_state(State state, uint8_t* out, size_t capacity);
size_t build_error(Error error, uint8_t* out, size_t capacity);

/// RPC result: data = [command, total length of the strings, (length, bytes)*]. `count` 0 builds
/// the empty result that ends a network list. Returns 0 if the strings do not fit one frame.
size_t build_rpc_result(uint8_t command, const char* const* strings, size_t count, uint8_t* out, size_t capacity);

/// Parses the payload of a wifi-settings RPC: [ssid length, ssid, psk length, psk] (the RPC data
/// after the command and its length byte). Writes NUL-terminated strings; false if the lengths do
/// not add up, the SSID is empty or a value does not fit its buffer (capacities include the NUL).
bool parse_wifi_settings(const uint8_t* payload, size_t length, char* ssid, size_t ssid_capacity, char* psk,
	size_t psk_capacity);

} // namespace improv

/// Splits a serial input stream into Improv frames and everything else (console input), byte by
/// byte. A frame starts with the magic "IMPROV"; bytes that match a prefix of it are held back,
/// and released as console bytes as soon as the match breaks (the breaking byte may start a new
/// magic: "IIMPROV" is "I" plus a frame). After the magic the version byte decides: 1 makes the
/// rest of the frame binary and consumed; anything else releases the held bytes as text.
///
/// A frame that stops arriving for kGapMs is given up: held magic bytes are released as text, a
/// partial frame past its version byte is dropped (bad_frame). A complete frame with a wrong
/// checksum is consumed and reported (bad_frame); the reader then looks for the next magic.
///
/// Not thread-safe; owned by one reader. No allocation, no logging.
class ImprovReader {
public:
	/// A frame whose bytes stop for this long is abandoned.
	static constexpr uint32_t kGapMs = 100;

	enum class Event : uint8_t {
		none,      ///< byte held (part of a possible frame)
		console,   ///< console bytes ready: console_data(), console_length()
		frame,     ///< a complete, valid frame: frame_type(), frame_data(), frame_length()
		bad_frame, ///< a frame was dropped: bad_reason()
	};

	/// Feeds one byte received at `now_ms`. The data returned by the accessors stays valid until
	/// the next call.
	Event feed(uint8_t byte, uint64_t now_ms);

	/// Gives up a frame whose bytes stopped arriving kGapMs ago. Call when no byte arrived.
	Event poll(uint64_t now_ms);

	/// Releases every held byte as console bytes (a partial frame past its version is dropped).
	Event flush();

	/// Whether bytes are held back.
	bool holding() const { return length_ > 0; }

	const uint8_t* console_data() const { return console_; }
	size_t console_length() const { return console_length_; }

	uint8_t frame_type() const { return frame_[7]; }
	const uint8_t* frame_data() const { return frame_ + improv::kHeaderSize; }
	size_t frame_length() const { return frame_[8]; }

	/// Why the last bad_frame was dropped ("checksum mismatch", "incomplete frame").
	const char* bad_reason() const { return bad_reason_; }

private:
	Event release_and_restart(uint8_t byte);
	void add_console(uint8_t byte);

	uint8_t frame_[improv::kMaxFrame] = {};
	size_t length_ = 0;
	size_t expected_ = 0;
	/// Released bytes: at most a held magic plus version and the byte that broke the match.
	uint8_t console_[improv::kHeaderSize] = {};
	size_t console_length_ = 0;
	uint64_t last_byte_ms_ = 0;
	const char* bad_reason_ = "";
};

} // namespace hc

#endif // HC_IMPROV_PROTOCOL_H
