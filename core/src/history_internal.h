/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The parts of hc::History that its two sources share: the ring and archive records and the
// conversions of the sample bits (history.cpp: rings, history_archive.cpp: archives).
#ifndef HC_HISTORY_INTERNAL_H
#define HC_HISTORY_INTERNAL_H

#include <cfloat>
#include <cstdint>
#include <cstring>

#include "hc/history.h"

// The samples and slots go to flash as they are in RAM: little-endian words, IEEE 754 numbers.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the history blob formats assume a little-endian CPU");
#endif

namespace hc {

namespace history_detail {

constexpr const char* kTag = "hc.history";

inline void put_u32(uint8_t* p, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

inline void put_u16(uint8_t* p, uint16_t v) {
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint32_t get_u32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		(static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t get_u16(const uint8_t* p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline bool sampled_type(ValueType type) {
	return type == ValueType::boolean || type == ValueType::unsigned_int || type == ValueType::signed_int ||
		type == ValueType::real;
}

inline uint32_t to_bits(const Value& value) {
	switch (value.type()) {
		case ValueType::boolean: return value.as_bool() ? 1 : 0;
		case ValueType::unsigned_int: return value.as_unsigned();
		case ValueType::signed_int: return static_cast<uint32_t>(value.as_signed());
		case ValueType::real: {
			double d = value.as_real();
			float f = d > FLT_MAX ? FLT_MAX : d < -FLT_MAX ? -FLT_MAX : static_cast<float>(d);
			uint32_t bits = 0;
			std::memcpy(&bits, &f, sizeof(bits));
			return bits;
		}
		default: return 0;
	}
}

inline double to_double(ValueType type, uint32_t bits) {
	switch (type) {
		case ValueType::boolean: return bits != 0 ? 1.0 : 0.0;
		case ValueType::unsigned_int: return bits;
		case ValueType::signed_int: return static_cast<int32_t>(bits);
		case ValueType::real: {
			float f = 0;
			std::memcpy(&f, &bits, sizeof(f));
			return f;
		}
		default: return 0;
	}
}

/// Whether a slot was fed at all.
inline bool slot_empty(const ArchiveSlot& slot) {
	return slot.count == 0 && slot.held_s == 0;
}

} // namespace history_detail

struct History::Ring {
	HistorySample* samples = nullptr;
	const char* name = ""; ///< Own copy of the datapoint's name.
	uint32_t capacity = 0;
	uint32_t head = 0;     ///< Index of the oldest sample.
	uint32_t count = 0;
	uint32_t spacing_s = 0;
	uint32_t retention_s = 0;
	uint32_t hash = 0;     ///< FNV-1a of the name (the blob names).
	uint16_t id = 0;
	ValueType type = ValueType::none;
	bool dirty = false;       ///< Samples added since the last save.
	bool persistable = true;  ///< False for the second of two names with the same hash.
	/// The archives, finest first (a range of History::archives_).
	Archive* archives = nullptr;
	uint8_t archive_count = 0;
	/// The archives hold the last sample's value up to this time (UTC seconds); 0: nothing to hold
	/// (no sample since the load).
	uint32_t credit_t = 0;

	HistorySample& at(uint32_t i) const { return samples[(head + i) % capacity]; }
	const HistorySample& back() const { return at(count - 1); }
};

struct History::Archive {
	uint32_t resolution_s = 0;
	uint32_t retention_s = 0;
	uint32_t slots = 0;       ///< The retention window: ring_samples(retention, resolution).
	uint32_t positions = 0;   ///< Slots on flash: segments * kSlotsPerSegment; slot n is at n % positions.
	uint16_t segments = 0;
	uint32_t days = 0;        ///< The resolution in whole local days; 0: aligned to UTC multiples.
	ArchiveSlot* pending = nullptr; ///< Closed slots not yet on flash, oldest first.
	uint16_t pending_capacity = 0;
	uint16_t pending_count = 0;
	bool has_open = false;
	bool open_dirty = false;  ///< The open slot changed since it was last written.
	uint32_t open_number = 0;
	int64_t open_end = 0;     ///< The open slot's end (exclusive), UTC seconds.
	ArchiveSlot open = {};
};

} // namespace hc

#endif // HC_HISTORY_INTERNAL_H
