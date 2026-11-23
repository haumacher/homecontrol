/* Copyright (c) 2026 Bernhard Haumacher */
// What the generic view draws of a history (web/ui/dist/history_view.js, homecontrol #115): the ranges
// a coverage reaches, the kind drawn, the span of a sparkline. Run by ctest as WebUi.HistoryView:
// `node --test web/ui/test/history_view.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { BARS, GENERIC_RANGES, LINE_POINTS, RANGE_SECONDS, consolidationOf, coveredRanges, drawKind, firstRange,
	hasSpark, sparkSeconds } from "../dist/history_view.js";

const DAY = 86400;

test("the sheet offers exactly the ranges the coverage reaches, at the levels E-Home records", () => {
	// The levels of the setting "Aufzeichnung" (haumacher/ehome#221) as /api/model gives their coverage.
	assert.deepEqual(coveredRanges(6 * 3600), ["6h"]);
	assert.deepEqual(coveredRanges(DAY), ["6h", "24h"]);
	assert.deepEqual(coveredRanges(7 * DAY), ["6h", "24h", "7d"]);
	assert.deepEqual(coveredRanges(30 * DAY), ["6h", "24h", "7d", "30d"]);
	assert.deepEqual(coveredRanges(365 * DAY), ["6h", "24h", "7d", "30d", "1y"]);
	assert.deepEqual(coveredRanges(3650 * DAY), GENERIC_RANGES);
	// Coverages between the levels (images of an older E-Home): what they reach, rounded down.
	assert.deepEqual(coveredRanges(2 * DAY), ["6h", "24h"]);
	assert.deepEqual(coveredRanges(31 * DAY), ["6h", "24h", "7d", "30d"]);
	assert.deepEqual(coveredRanges(2 * 365 * DAY), ["6h", "24h", "7d", "30d", "1y"]);
	// Shorter than the shortest range: that one, filled as far as the history reaches.
	assert.deepEqual(coveredRanges(3600), ["6h"]);
	// No history: nothing.
	assert.deepEqual(coveredRanges(0), []);
	assert.deepEqual(coveredRanges(undefined), []);
	// The range "1h" stays valid on the pages of older images, but the generic view does not offer it.
	assert.equal(RANGE_SECONDS["1h"], 3600);
	assert.ok(!GENERIC_RANGES.includes("1h"));
});

test("the first range is 24 h where it is offered, else the shortest", () => {
	assert.equal(firstRange(coveredRanges(365 * DAY)), "24h");
	assert.equal(firstRange(coveredRanges(DAY)), "24h");
	assert.equal(firstRange(coveredRanges(6 * 3600)), "6h");
});

test("a number is drawn as its line with the band, a boolean as the strip, a meter as bars", () => {
	assert.equal(drawKind({ type: "real", history: true, meter: false }), "band");
	assert.equal(drawKind({ type: "unsigned", history: true }), "band"); // an older firmware: no meter member
	assert.equal(drawKind({ type: "boolean", history: true, meter: false }), "strip");
	assert.equal(drawKind({ type: "signed", history: true, meter: true }), "bars");
	assert.equal(drawKind({ type: "real", history: true, meter: true }), "bars");
	assert.equal(consolidationOf({ type: "real", meter: false }), "band");
	assert.equal(consolidationOf({ type: "boolean" }), "mean");
	assert.equal(consolidationOf({ type: "unsigned", meter: true }), "consumption");
});

test("the ranges 6h and 10y: 48 points, and their bars of 15 min and of a year", () => {
	assert.equal(LINE_POINTS, 48);
	assert.equal(RANGE_SECONDS["6h"], 21600);
	assert.equal(RANGE_SECONDS["10y"], 3650 * DAY);
	assert.deepEqual(BARS["6h"], [24, 900]);
	assert.deepEqual(BARS["10y"], [10, 365 * DAY]);
	// The bars cover their range.
	for (const range of Object.keys(BARS)) {
		const [count, width] = BARS[range];
		assert.equal(Math.round(count * width), RANGE_SECONDS[range], range);
	}
	// A line of 48 points over 6 h: 7.5 minutes a bucket, so the ring of 5 min answers it.
	assert.equal(RANGE_SECONDS["6h"] / LINE_POINTS, 450);
});

test("every tile whose value keeps a history has a sparkline, over 24 h or the coverage if shorter", () => {
	assert.equal(hasSpark({ type: "boolean", history: true, coverage: DAY }), true);
	assert.equal(hasSpark({ type: "real", history: false }), false);
	assert.equal(hasSpark(undefined), false);
	assert.equal(sparkSeconds({ history: true, coverage: 365 * DAY }), DAY);
	assert.equal(sparkSeconds({ history: true, coverage: 6 * 3600 }), 6 * 3600);
	assert.equal(sparkSeconds({ history: true }), DAY); // an older firmware without coverage
});
