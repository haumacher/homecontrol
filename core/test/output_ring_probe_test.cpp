/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the console output ring (issue #46) composed with the way the device uses it: lines
// arriving in fragments from several printf calls, Improv records between them, a slow consumer
// taking a few bytes at a time across hundreds of wraps, and the drop marker in the middle of a
// burst. The stream the consumer sees must be the lines and records in order, byte for byte.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/output_ring.h"

namespace {

/// Takes `step` bytes at a time until the ring is empty; appends them to `out`.
void drain(hc::OutputRing& ring, std::string& out, size_t step) {
	for (;;) {
		const uint8_t* data = nullptr;
		size_t n = ring.peek(&data);
		if (n == 0) {
			return;
		}
		size_t take = n < step ? n : step;
		out.append(reinterpret_cast<const char*>(data), take);
		ring.consume(take);
	}
}

} // namespace

TEST_CASE("OutputRingProbe: fragments, records and a 7-byte consumer across many wraps keep the stream exact") {
	uint8_t storage[128];
	hc::OutputRing ring(storage, sizeof(storage), true);
	std::string expected;
	std::string seen;
	for (int i = 0; i < 300; ++i) {
		// One line printed in three fragments, as printf("%s", ...) calls of one task would do.
		std::string line = "I (" + std::to_string(i * 7) + ") hc.probe: line " + std::to_string(i);
		CHECK(ring.append_text(line.data(), 10) == 0);
		CHECK(ring.append_text(line.data() + 10, line.size() - 10) == 0);
		CHECK(ring.append_text("\n", 1) == 0);
		expected += line + "\r\n";
		if (i % 5 == 0) {
			uint8_t frame[] = {0x49, 0x4d, 0x50, 0x52, 0x4f, 0x56, 0x01, 0x0a, 0x0d, static_cast<uint8_t>(i), 0x0a};
			CHECK(ring.append_record(frame, sizeof(frame)) == 0);
			expected.append(reinterpret_cast<const char*>(frame), sizeof(frame));
		}
		// The consumer keeps up in small steps, so nothing is dropped.
		drain(ring, seen, 7);
	}
	CHECK(ring.dropped() == 0);
	CHECK(seen == expected);
}

TEST_CASE("OutputRingProbe: a burst beyond the capacity drops whole lines, the marker appears once in order") {
	uint8_t storage[200];
	hc::OutputRing ring(storage, sizeof(storage), true);
	// Each line is 40 bytes with CRLF; the ring takes 5 of them, the sixth and seventh are dropped.
	std::vector<std::string> lines;
	uint32_t dropped = 0;
	for (int i = 0; i < 7; ++i) {
		std::string line = "W (1000) hc.loop: burst line number " + std::to_string(i) + "\n";
		REQUIRE(line.size() == 38); // 39 with the CRLF: five fit in 200 bytes
		dropped += ring.append_text(line.data(), line.size());
		lines.push_back(line.substr(0, line.size() - 1) + "\r\n");
	}
	CHECK(dropped == 2);
	CHECK(ring.dropped() == 2);
	// A record that does not fit is dropped as one line as well, and counted.
	uint8_t frame[64] = {};
	CHECK(ring.append_record(frame, sizeof(frame)) == 1);
	CHECK(ring.dropped() == 3);
	std::string seen;
	drain(ring, seen, 1000);
	// The five lines, then the marker (the ring ran empty), in this order and nothing else.
	std::string expected = lines[0] + lines[1] + lines[2] + lines[3] + lines[4];
	REQUIRE(seen.size() > expected.size());
	CHECK(seen.substr(0, expected.size()) == expected);
	std::string marker = seen.substr(expected.size());
	CHECK(marker.find("3 line(s) dropped") != std::string::npos);
	CHECK(marker.find("\r\n") == marker.size() - 2);
	CHECK(ring.unreported() == 0);
	// Output after the marker is plain again.
	CHECK(ring.append_text("after\n", 6) == 0);
	seen.clear();
	drain(ring, seen, 1000);
	CHECK(seen == "after\r\n");
}

TEST_CASE("OutputRingProbe: a line whose head went in and whose tail does not fit is still completed, later lines drop") {
	uint8_t storage[64];
	hc::OutputRing ring(storage, sizeof(storage), false);
	std::string head(40, 'a');
	CHECK(ring.append_text(head.data(), head.size()) == 0); // partial, no newline yet
	std::string tail(40, 'b');
	tail += "\n";
	uint32_t dropped = ring.append_text(tail.data(), tail.size());
	// Whatever the ring decided for this line, the consumer must not see a torn line later: either
	// the whole line or the head alone followed by the marker, never a tail without its head.
	std::string seen;
	drain(ring, seen, 5);
	bool whole = seen.rfind(head + tail, 0) == 0;
	bool head_only = seen.rfind(head, 0) == 0 && seen.find('b') == std::string::npos;
	CHECK((whole || head_only));
	if (head_only) {
		CHECK(dropped == 1);
		CHECK(seen.find("1 line(s) dropped") != std::string::npos);
	} else {
		CHECK(dropped == 0);
	}
	CHECK(ring.append_text("next\n", 5) == 0);
	seen.clear();
	drain(ring, seen, 5);
	CHECK(seen == "next\n");
}
