/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/log_ring.h"
#include "test_support.h"

namespace {

struct Collector : hc::LogLineVisitor {
	bool line(const hc::LogLine& line) override {
		seqs.push_back(line.seq);
		texts.push_back(std::string(hc::log_level_letter(line.level)) + " " + line.tag + ": " + line.message);
		ms.push_back(line.ms);
		return limit == 0 || seqs.size() < limit;
	}
	std::vector<uint32_t> seqs;
	std::vector<std::string> texts;
	std::vector<uint64_t> ms;
	size_t limit = 0;
};

} // namespace

TEST_CASE("LogRing keeps lines in order with their time and sequence") {
	hc_test::FakeClock clock;
	hc::LogRing ring(&clock);
	ring.append(hc::LogLevel::info, "hc.a", "first");
	clock.advance(5);
	ring.append(hc::LogLevel::error, "hc.b", "second");
	Collector all;
	CHECK(ring.read(0, all) == 2);
	CHECK(all.texts == std::vector<std::string>{"I hc.a: first", "E hc.b: second"});
	CHECK(all.seqs == std::vector<uint32_t>{1, 2});
	CHECK(all.ms[1] - all.ms[0] == 5);
	Collector after;
	CHECK(ring.read(1, after) == 1);
	CHECK(after.texts[0] == "E hc.b: second");
	CHECK(ring.first_seq() == 1);
	CHECK(ring.last_seq() == 2);
}

TEST_CASE("LogRing drops the oldest lines when full and wraps correctly") {
	hc::LogRing ring;
	std::string text(100, 'x');
	for (int i = 0; i < 200; ++i) {
		text[0] = static_cast<char>('a' + i % 26);
		ring.append(hc::LogLevel::info, "hc.t", text.c_str());
	}
	Collector all;
	size_t n = ring.read(0, all);
	CHECK(n == ring.count());
	CHECK(n > 20);
	CHECK(n < 40); // 4 KB of ~125-byte entries
	CHECK(all.seqs.back() == 200);
	for (size_t i = 1; i < all.seqs.size(); ++i) {
		CHECK(all.seqs[i] == all.seqs[i - 1] + 1);
	}
	CHECK(ring.first_seq() == all.seqs.front());
	char expected = static_cast<char>('a' + 199 % 26);
	CHECK(all.texts.back()[8] == expected);
	// Very long lines are cut, not refused.
	ring.append(hc::LogLevel::warn, "hc.t", std::string(1000, 'y').c_str());
	Collector last;
	ring.read(ring.last_seq() - 1, last);
	CHECK(last.texts.back().size() < hc::LogRing::kMaxLine + 8);
	// The visitor can stop early.
	Collector limited;
	limited.limit = 3;
	CHECK(ring.read(0, limited) == 3);
}

TEST_CASE("LogRing copy is consistent and the controller mirrors hc::Log into its ring") {
	hc_test::FakePlatform fake;
	hc::Controller controller(fake.platform());
	hc::Log::info("hc.test", "mirrored %d", 42);
	hc::LogRing copy;
	REQUIRE(controller.log_ring().copy_to(copy));
	Collector lines;
	copy.read(0, lines);
	REQUIRE_FALSE(lines.texts.empty());
	CHECK(lines.texts.back() == "I hc.test: mirrored 42");
	CHECK(lines.ms.back() == fake.clock.now_ms);
	CHECK(copy.dropped() == 0);
}
