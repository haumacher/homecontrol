/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_HOUSE_MODE_H
#define HC_BLOCKS_HOUSE_MODE_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The house mode `org.homecontrol.house-mode` 1.0 (spec/blocks.md 19): home, away, night or vacation
/// from presence, push buttons, the morning and evening of schedule entries and a vacation switch;
/// the mode as a KNX HVAC mode for the room controllers and as booleans; persisted across a reboot.
class HouseMode : public BlockType {
public:
	// Ports, in manifest order.
	enum Port : uint8_t {
		kPresence, kAwayButton, kNightButton, kHomeButton, kVacation, kMorning, kEvening,
		kMode, kHvacMode, kHome, kAway, kNight, kOnVacation, kOccupied, kSince,
		kPortCount
	};
	// Parameters, in manifest order.
	enum Param : uint8_t {
		kAwayDelay, kReturnDetection, kHvacHome, kHvacAway, kHvacNight, kHvacVacation,
		kParamCount
	};
	// State fields, in manifest order.
	enum Field : uint8_t {
		kModeField, kSinceField, kChangedAt, kAbsentSince, kArmed, kAwaySeen, kNightSeen, kHomeSeen, kLastMorning,
		kLastEvening, kSentMode, kSentSince,
		kFieldCount
	};
	/// The values of the output `mode`.
	enum Mode : uint8_t { home, away, night, vacation };
	/// What a call hands the step (spec/blocks.md 19.4).
	enum Event : uint8_t { none, away_press, night_press, home_press, morning, evening };

	static const HouseMode& instance();

	const Manifest& manifest() const override;
	void start(BlockContext& cx, StartReason reason, bool restored) const override;
	void input(BlockContext& cx, size_t port) const override;
	void timer(BlockContext& cx, size_t timer) const override;
	void period(BlockContext& cx) const override;
	void parameter(BlockContext& cx, size_t index) const override;

private:
	void step(BlockContext& cx, Event event) const;
};

} // namespace hc::blocks

#endif // HC_BLOCKS_HOUSE_MODE_H
