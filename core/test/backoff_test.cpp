/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <vector>

#include "doctest.h"
#include "hc/backoff.h"

using hc::Backoff;

TEST_CASE("Backoff.DefaultSequenceDoublesUpToSixtySeconds") {
	Backoff backoff;
	std::vector<uint32_t> delays;
	for (int i = 0; i < 9; ++i) {
		delays.push_back(backoff.next());
	}
	CHECK(delays == std::vector<uint32_t>{1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000});
	CHECK(backoff.failures() == 9);
}

TEST_CASE("Backoff.ResetStartsOver") {
	Backoff backoff;
	backoff.next();
	backoff.next();
	backoff.next();
	CHECK(backoff.peek() == 8000);
	backoff.reset();
	CHECK(backoff.failures() == 0);
	CHECK(backoff.peek() == 1000);
	CHECK(backoff.next() == 1000);
	CHECK(backoff.next() == 2000);
}

TEST_CASE("Backoff.CustomInitialAndCap") {
	Backoff backoff(300, 1000);
	CHECK(backoff.next() == 300);
	CHECK(backoff.next() == 600);
	CHECK(backoff.next() == 1000);
	CHECK(backoff.next() == 1000);
}

TEST_CASE("Backoff.DegenerateParametersAreRepaired") {
	Backoff zero(0, 0);
	CHECK(zero.initial_ms() == 1);
	CHECK(zero.next() == 1);
	CHECK(zero.next() == 1);

	Backoff low_cap(5000, 1000);
	CHECK(low_cap.cap_ms() == 5000);
	CHECK(low_cap.next() == 5000);
	CHECK(low_cap.next() == 5000);
}

TEST_CASE("Backoff.HugeCapDoesNotOverflow") {
	Backoff backoff(UINT32_MAX / 2 + 1, UINT32_MAX);
	CHECK(backoff.next() == UINT32_MAX / 2 + 1);
	CHECK(backoff.next() == UINT32_MAX);
	CHECK(backoff.next() == UINT32_MAX);
}
