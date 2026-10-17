/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_DPT_H
#define HC_DPT_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/datapoint.h"

namespace hc {

/// A KNX datapoint type by its numbers, e.g. 9.001 (main 9, sub 1). The sub number 0 means the
/// subtype x.000 where the standard defines one (16.000 ASCII string, 14.000) and the main type in
/// general otherwise: such a type and an unknown subtype use the rules of the main type.
struct Dpt {
	uint16_t main = 0;
	uint16_t sub = 0;

	/// Longest text format() writes, including the NUL ("65535.65535").
	static constexpr size_t kMaxText = 12;

	/// Reads "9.001", "9.1", "9", "9.*", "DPST-9-1" or "DPT-9". Empty for anything else.
	static std::optional<Dpt> parse(const char* text);

	/// Writes "9.001" (at least three digits for the sub number). Returns `buffer`.
	const char* format(char* buffer, size_t size) const;

	/// Whether encode() and decode() support this type (unsupported_reason() says why not).
	bool supported() const;

	/// Why the type is not supported; nullptr if it is.
	const char* unsupported_reason() const;

	/// Whether the value travels in the low 6 bits of the APCI octet of a group telegram (the types
	/// of up to 6 bits: main types 1, 2 and 3). encode() then writes one octet holding those bits,
	/// decode() reads one; the KNX adapter merges it into the APCI octet (data length 0).
	bool embedded() const;

	/// Octets of the encoded value: 1 for an embedded type (the octet with the 6 bits), 0 for an
	/// unsupported type.
	size_t size() const;

	/// The value type decode() produces and encode() expects (none for an unsupported type).
	ValueType value_type() const;

	friend bool operator==(Dpt a, Dpt b) { return a.main == b.main && a.sub == b.sub; }
	friend bool operator!=(Dpt a, Dpt b) { return !(a == b); }
};

/// Outcome of encode() and decode(). Everything except `ok` is a refusal; the DptResult carries the
/// reason text to log.
enum class DptStatus : uint8_t {
	ok,
	unsupported,  ///< The datapoint type is not implemented.
	wrong_length, ///< decode(): the data length does not match the type.
	out_of_range, ///< encode(): the value lies outside the type's range (never clamped).
	invalid,      ///< decode(): the data is no value of the type (a reserved code, a month 13, 0x7FFF).
	wrong_type,   ///< encode(): the value does not convert to the type's value type.
};

/// "ok", "unsupported", "wrong_length", "out_of_range", "invalid", "wrong_type" (the names of the
/// conformance corpus, spec/dpt-corpus.md).
const char* dpt_status_name(DptStatus status);

/// The outcome of a codec call with a reason for a refusal (a static text, e.g. "year outside
/// 1990..2089").
struct DptResult {
	DptStatus status = DptStatus::ok;
	const char* reason = "";

	explicit operator bool() const { return status == DptStatus::ok; }
};

/// The encoded data of one value, as it follows the APCI in a group telegram (for an embedded type:
/// the one octet with the 6 bits).
struct DptBytes {
	static constexpr size_t kCapacity = 14;

	uint8_t data[kCapacity] = {};
	uint8_t length = 0;
};

/// Encodes `value` as `dpt`. The value is converted to dpt.value_type() where exact (an integer
/// for a real type, a real holding an integer for an integer type). A value outside the type's
/// range is refused, never clamped; scaled types (5.001, 5.003, 7.003, 8.010, 9.x, 14.x) round to
/// the nearest representable value. On a refusal `bytes` is empty.
DptResult encode(Dpt dpt, const Value& value, DptBytes& bytes);

/// Decodes the data of a telegram. The length must be exactly dpt.size(). Reserved bits are ignored
/// (as the receiving side of the standard requires); field values outside their range and codes
/// the type declares invalid are refused. On a refusal `value` is empty.
DptResult decode(Dpt dpt, const uint8_t* data, size_t length, Value& value);

inline DptResult decode(Dpt dpt, const DptBytes& bytes, Value& value) {
	return decode(dpt, bytes.data, bytes.length, value);
}

} // namespace hc

#endif // HC_DPT_H
