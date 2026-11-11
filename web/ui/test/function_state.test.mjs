/* Copyright (c) 2026 Bernhard Haumacher */
// What a function shows (web/ui/dist/function_state.js, spec/runtime-image.md 3.5, schema 1.6), run
// by ctest as WebUi.FunctionState: `node --test web/ui/test/function_state.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { blindState, dimmerOn, flames, heating, END_TOLERANCE } from "../dist/function_state.js";

test("a blind with only its end positions shows open, closed or between, never a percentage", () => {
	assert.deepEqual(blindState(null, true, false), { kind: "open", conflict: false });
	assert.deepEqual(blindState(null, false, true), { kind: "closed", conflict: false });
	// Stopped on the way: neither end reports.
	assert.deepEqual(blindState(null, false, false), { kind: "between", conflict: false });
	assert.deepEqual(blindState(null, null, null), { kind: "unknown", conflict: false });
	assert.deepEqual(blindState(null, true, true), { kind: "unknown", conflict: true });
	for (const top of [true, false, null]) {
		for (const bottom of [true, false, null]) {
			assert.equal(blindState(null, top, bottom).percent, undefined);
		}
	}
});

test("a blind with one end position bound claims no more than its bit says", () => {
	assert.deepEqual(blindState(null, true, null), { kind: "open", conflict: false });
	assert.deepEqual(blindState(null, false, null), { kind: "notOpen", conflict: false });
	assert.deepEqual(blindState(null, null, true), { kind: "closed", conflict: false });
	assert.deepEqual(blindState(null, null, false), { kind: "notClosed", conflict: false });
});

test("a blind's position wins over its end positions, which only cross-check it", () => {
	assert.deepEqual(blindState(35, false, false), { kind: "position", percent: 35, conflict: false });
	assert.deepEqual(blindState(0, true, false), { kind: "position", percent: 0, conflict: false });
	assert.deepEqual(blindState(100, false, true), { kind: "position", percent: 100, conflict: false });
	assert.deepEqual(blindState(END_TOLERANCE, true, false), { kind: "position", percent: END_TOLERANCE, conflict: false });
	// The end reports contradict the position: still the position, marked.
	assert.deepEqual(blindState(40, true, false), { kind: "position", percent: 40, conflict: true });
	assert.deepEqual(blindState(40, false, true), { kind: "position", percent: 40, conflict: true });
	assert.deepEqual(blindState(100, true, true), { kind: "position", percent: 100, conflict: true });
	// Unknown end positions do not contradict.
	assert.deepEqual(blindState(60, null, null), { kind: "position", percent: 60, conflict: false });
});

test("heating of a boolean, of a percentage (above 0) and from the heating level", () => {
	assert.equal(heating(true, null), true);
	assert.equal(heating(false, 80), false); // the heating role decides where it has a value
	assert.equal(heating(35, null), true);
	assert.equal(heating(0, null), false);
	assert.equal(heating(0.4, 0.4), true);
	assert.equal(heating(null, 12), true);
	assert.equal(heating(null, 0), false);
	assert.equal(heating(null, null), null);
});

test("the flame steps of a heating level", () => {
	assert.equal(flames(null), null);
	assert.equal(flames(0), 0);
	assert.equal(flames(1), 1);
	assert.equal(flames(33.3), 1);
	assert.equal(flames(34), 2);
	assert.equal(flames(66.6), 2);
	assert.equal(flames(67), 3);
	assert.equal(flames(100), 3);
});

test("a dimmer with its switching command alone is on or off by its command", () => {
	assert.equal(dimmerOn(null, true, null), true);
	assert.equal(dimmerOn(null, false, null), false);
	assert.equal(dimmerOn(null, null, null), null);
	assert.equal(dimmerOn(false, true, 50), false); // the state wins
	assert.equal(dimmerOn(null, false, 50), true);  // a level above 0 is on
	assert.equal(dimmerOn(null, null, 0), false);
});
