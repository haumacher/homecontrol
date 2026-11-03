/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_CONFORMANCE_BLOCK_H
#define HC_TEST_CONFORMANCE_BLOCK_H

#include "hc/blocks/block_type.h"

namespace hc_test {

/// The conformance type `org.homecontrol.conformance` of spec/blocks.md 15, in versions 1.1 and 2.0:
/// a block simple enough to compute by hand, that faults on demand. Only the test harnesses have it
/// (decision B18); E-Home's simulation registers its Java twin for the corpus.
class ConformanceBlock : public hc::blocks::BlockType {
public:
	enum Port : uint8_t { kX, kArm, kTick, kY, kPulse, kCalls, kTicks };
	enum Field : uint8_t { kCount, kTicksField, kPulseOn };
	enum Param : uint8_t { kGain, kOffset, kFaultAbove, kFourth }; ///< kFourth: `delay` (1.1) or `level` (2.0).

	static const ConformanceBlock& v1();
	static const ConformanceBlock& v2();

	const hc::blocks::Manifest& manifest() const override { return manifest_; }
	void start(hc::blocks::BlockContext& cx, hc::blocks::StartReason reason, bool restored) const override;
	void input(hc::blocks::BlockContext& cx, size_t port) const override;
	void timer(hc::blocks::BlockContext& cx, size_t timer) const override;
	void period(hc::blocks::BlockContext& cx) const override;
	void parameter(hc::blocks::BlockContext& cx, size_t index) const override;

private:
	explicit ConformanceBlock(const hc::blocks::Manifest& manifest, bool second) : manifest_(manifest), second_(second) {}
	void update(hc::blocks::BlockContext& cx) const;

	const hc::blocks::Manifest& manifest_;
	bool second_;
};

/// A registry with the device's built-in types and both conformance versions.
hc::blocks::BlockRegistry& corpus_registry();

} // namespace hc_test

#endif // HC_TEST_CONFORMANCE_BLOCK_H
