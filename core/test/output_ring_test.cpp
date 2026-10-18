/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The console output queue (issue #46): append from a producer, drain by a consumer, a full ring
// drops whole lines and counts them, the marker line appears once when output resumes, records
// (Improv frames) stay whole.
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/output_ring.h"

namespace {

/// Takes everything queued (as the writer task does, run by run).
std::string drain(hc::OutputRing& ring, size_t max_run = 1000000) {
	std::string out;
	for (;;) {
		const uint8_t* data = nullptr;
		size_t n = ring.peek(&data);
		if (n == 0) {
			return out;
		}
		if (n > max_run) {
			n = max_run;
		}
		out.append(reinterpret_cast<const char*>(data), n);
		ring.consume(n);
	}
}

uint32_t text(hc::OutputRing& ring, const char* s) {
	return ring.append_text(s, std::strlen(s));
}

const char* kMarker1 = "\xE2\x80\xA6 1 line(s) dropped (console output could not keep up)\n";

} // namespace

TEST_CASE("OutputRing: producer and consumer, across the wrap") {
	uint8_t storage[16];
	hc::OutputRing ring(storage, sizeof(storage), false);
	CHECK(text(ring, "hello\n") == 0);
	CHECK(text(ring, "world\n") == 0);
	CHECK(ring.queued() == 12);
	CHECK(drain(ring, 5) == "hello\nworld\n");
	// The next lines wrap around the end of the storage; peek() hands out the run before the wrap.
	CHECK(text(ring, "abcdefgh\n") == 0);
	const uint8_t* data = nullptr;
	REQUIRE(ring.peek(&data) == 9);
	ring.consume(4);
	CHECK(text(ring, "0123456789\n") == 0);
	CHECK(ring.queued() == 16);
	CHECK(drain(ring) == "efgh\n0123456789\n");
	CHECK(ring.dropped() == 0);
	CHECK(ring.capacity() == 16);
}

TEST_CASE("OutputRing: CRLF translation of text, records as they are") {
	uint8_t storage[64];
	hc::OutputRing ring(storage, sizeof(storage), true);
	CHECK(text(ring, "a\nb\n") == 0);
	const uint8_t frame[] = {'\n', 'I', 'M', 'P', 'R', 'O', 'V', 0x0a, 0x00, '\n'};
	CHECK(ring.append_record(frame, sizeof(frame)) == 0);
	CHECK(drain(ring) == std::string("a\r\nb\r\n") + std::string(reinterpret_cast<const char*>(frame), sizeof(frame)));
}

TEST_CASE("OutputRing: a full ring drops whole lines, counts them, and reports them once") {
	uint8_t storage[100];
	hc::OutputRing ring(storage, sizeof(storage), false);
	const char* line40 = "0123456789012345678901234567890123456789\n"; // 41 bytes
	CHECK(text(ring, line40) == 0);
	CHECK(text(ring, line40) == 0); // 82 queued, 18 free
	// The third line does not fit: dropped and counted, what is queued stays intact.
	CHECK(text(ring, "this line does not fit\n") == 1);
	CHECK(ring.dropped() == 1);
	CHECK(ring.unreported() == 1);
	// A short line would fit, but the marker has to go first and both do not: dropped too, one by one.
	CHECK(text(ring, "x1\nx2\nx3\n") == 3);
	CHECK(ring.dropped() == 4);
	CHECK(ring.unreported() == 4);
	CHECK(ring.queued() == 82);

	// The consumer takes one line; the next line goes in behind the marker, which comes once.
	const uint8_t* data = nullptr;
	REQUIRE(ring.peek(&data) == 82);
	ring.consume(41);
	ring.consume(41);
	CHECK(text(ring, "resumed\n") == 0);
	CHECK(ring.unreported() == 0);
	CHECK(text(ring, "next\n") == 0);
	CHECK(drain(ring) == std::string("\xE2\x80\xA6 4 line(s) dropped (console output could not keep up)\nresumed\nnext\n"));
	CHECK(ring.dropped() == 4); // the total stays

	// A later overflow is reported with its own count.
	for (int i = 0; i < 3; ++i) {
		text(ring, line40);
	}
	CHECK(ring.unreported() == 1);
	CHECK(ring.dropped() == 5);
}

TEST_CASE("OutputRing: when nothing follows, the consumer gets the marker once the ring has run empty") {
	uint8_t storage[100];
	hc::OutputRing ring(storage, sizeof(storage), false);
	const char* line60 = "012345678901234567890123456789012345678901234567890123456789\n"; // 61 bytes
	CHECK(text(ring, line60) == 0);
	CHECK(text(ring, line60) == 1);
	CHECK(drain(ring) == std::string(line60) + kMarker1);
	CHECK(ring.unreported() == 0);
	const uint8_t* data = nullptr;
	CHECK(ring.peek(&data) == 0); // once
}

TEST_CASE("OutputRing: a line is never cut: once part of it is dropped, its rest is dropped too") {
	uint8_t storage[100];
	hc::OutputRing ring(storage, sizeof(storage), false);
	const char* line60 = "012345678901234567890123456789012345678901234567890123456789\n";
	CHECK(text(ring, line60) == 0); // 39 free
	// A line written in three parts (stdio flushes a full buffer mid-line): its head fits, its
	// middle does not; the tail is dropped with it although it would fit, and the line counts once.
	CHECK(text(ring, "head ") == 0);
	CHECK(text(ring, "a middle part that is much too long for the ring ") == 1);
	CHECK(text(ring, "tail\n") == 0);
	CHECK(ring.dropped() == 1);
	// The marker starts on a line of its own after the cut line's head.
	CHECK(drain(ring) == std::string(line60) + "head \n" + kMarker1);
	// The next line arrives whole.
	CHECK(text(ring, "whole\n") == 0);
	CHECK(drain(ring) == "whole\n");

	// A partial line that is dropped (a prompt without "\n") counts at once, and its rest later
	// does not count again.
	uint8_t small[80];
	hc::OutputRing tiny(small, sizeof(small), false);
	CHECK(text(tiny, line60) == 0);
	CHECK(text(tiny, "a prompt that does not fit> ") == 1);
	CHECK(tiny.dropped() == 1);
	CHECK(text(tiny, "typed\n") == 0);
	CHECK(tiny.dropped() == 1);
}

TEST_CASE("OutputRing: records are whole or dropped as one line, and wait behind the marker") {
	uint8_t storage[100];
	hc::OutputRing ring(storage, sizeof(storage), true);
	uint8_t frame[50];
	std::memset(frame, 0x0a, sizeof(frame)); // binary bytes that look like line ends: not translated
	CHECK(ring.append_record(frame, sizeof(frame)) == 0);
	CHECK(ring.append_record(frame, sizeof(frame)) == 0);
	CHECK(ring.queued() == 100);
	CHECK(ring.append_record(frame, sizeof(frame)) == 1);
	CHECK(ring.dropped() == 1);
	std::string out = drain(ring);
	CHECK(out.size() == 100 + std::strlen(kMarker1) + 1); // the marker with CRLF
	CHECK(out.substr(100) == "\xE2\x80\xA6 1 line(s) dropped (console output could not keep up)\r\n");
	// A record larger than the ring can never go in, and says so.
	uint8_t big[101] = {};
	CHECK(ring.append_record(big, sizeof(big)) == 1);
	CHECK(ring.dropped() == 2);
	// Interleaving: text lines and a frame keep their order, the frame never inside a line.
	drain(ring);
	CHECK(text(ring, "log\n") == 0);
	CHECK(ring.append_record(frame, 3) == 0);
	CHECK(text(ring, "more\n") == 0);
	CHECK(drain(ring) == std::string("log\r\n") + std::string(3, '\n') + "more\r\n");
}

TEST_CASE("OutputRing: the marker's room is reckoned exactly, for any count") {
	for (uint32_t drops : {1u, 9u, 10u, 12345u}) {
		std::string marker = "\xE2\x80\xA6 " + std::to_string(drops) + " line(s) dropped (console output could not keep up)\r\n";
		for (size_t extra : {0u, 1u}) {
			uint8_t storage[200];
			hc::OutputRing ring(storage, sizeof(storage), true);
			uint8_t record[200] = {};
			record[sizeof(record) - 1] = '\n'; // ends a line: the marker needs no line break of its own
			REQUIRE(ring.append_record(record, sizeof(record)) == 0);
			for (uint32_t i = 0; i < drops; ++i) {
				ring.append_record(record, 1);
			}
			REQUIRE(ring.unreported() == drops);
			const uint8_t* data = nullptr;
			REQUIRE(ring.peek(&data) == sizeof(record));
			ring.consume(sizeof(record)); // empty, the drops not yet reported
			// A line of exactly the room behind the marker goes in; one byte more is dropped.
			size_t room = sizeof(storage) - marker.size();
			std::string line(room - 2 + extra, 'x');
			line += "\n"; // written as CRLF
			if (extra == 0) {
				CHECK(text(ring, line.c_str()) == 0);
				CHECK(ring.queued() == sizeof(storage));
				CHECK(drain(ring) == marker + std::string(room - 2, 'x') + "\r\n");
			} else {
				CHECK(text(ring, line.c_str()) == 1);
				CHECK(ring.unreported() == drops + 1);
			}
		}
	}
}
