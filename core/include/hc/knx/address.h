/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_ADDRESS_H
#define HC_KNX_ADDRESS_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace hc {
namespace knx {

/// A KNX group address: 16 bits, written in three levels main/middle/sub (5/3/8 bits, "1/2/3") or
/// two levels main/sub (5/11 bits, "1/515"). Parsing accepts both, formatting writes three levels.
/// 0/0/0 is the broadcast address; it is a valid address but never a datapoint's.
struct GroupAddress {
	uint16_t raw = 0;

	/// Longest text format() writes, including the NUL ("31/7/255").
	static constexpr size_t kMaxText = 10;

	static constexpr GroupAddress of(uint8_t main, uint8_t middle, uint8_t sub) {
		return GroupAddress{static_cast<uint16_t>(((main & 0x1F) << 11) | ((middle & 0x07) << 8) | sub)};
	}

	/// Reads "1/2/3" (main 0..31, middle 0..7, sub 0..255) or "1/515" (main 0..31, sub 0..2047).
	/// Empty on anything else; `error` (if given) then names the problem.
	static std::optional<GroupAddress> parse(const char* text, const char** error = nullptr);

	/// Writes "1/2/3". Returns `buffer`.
	const char* format(char* buffer, size_t size) const;

	uint8_t main() const { return static_cast<uint8_t>(raw >> 11); }
	uint8_t middle() const { return static_cast<uint8_t>((raw >> 8) & 0x07); }
	uint8_t sub() const { return static_cast<uint8_t>(raw & 0xFF); }

	friend bool operator==(GroupAddress a, GroupAddress b) { return a.raw == b.raw; }
	friend bool operator!=(GroupAddress a, GroupAddress b) { return a.raw != b.raw; }
	friend bool operator<(GroupAddress a, GroupAddress b) { return a.raw < b.raw; }
};

/// A KNX individual address area.line.device ("1.1.250"; 4/4/8 bits).
struct IndividualAddress {
	uint16_t raw = 0;

	/// Longest text format() writes, including the NUL ("15.15.255").
	static constexpr size_t kMaxText = 10;

	static constexpr IndividualAddress of(uint8_t area, uint8_t line, uint8_t device) {
		return IndividualAddress{static_cast<uint16_t>(((area & 0x0F) << 12) | ((line & 0x0F) << 8) | device)};
	}

	/// Reads "1.1.250" (area 0..15, line 0..15, device 0..255). Empty on anything else; `error` (if
	/// given) then names the problem.
	static std::optional<IndividualAddress> parse(const char* text, const char** error = nullptr);

	/// Writes "1.1.250". Returns `buffer`.
	const char* format(char* buffer, size_t size) const;

	friend bool operator==(IndividualAddress a, IndividualAddress b) { return a.raw == b.raw; }
	friend bool operator!=(IndividualAddress a, IndividualAddress b) { return a.raw != b.raw; }
};

} // namespace knx
} // namespace hc

#endif // HC_KNX_ADDRESS_H
