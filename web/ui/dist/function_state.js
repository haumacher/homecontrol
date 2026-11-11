/* Copyright (c) 2026 Bernhard Haumacher */
// What a function shows, derived from the values of its roles (spec/runtime-image.md 3.5, "What the
// controller shows", schema 1.6). Pure functions over plain values: a number or a boolean, null for
// a role that is not bound or whose value is not known. Tested by web/ui/test/function_state.test.mjs
// (ctest WebUi.FunctionState).

/// The tolerance of the cross-check of a position against the end positions, in percent: an actuator
/// reports its end position with the last percent of its run, a position of 2 % at the top is no
/// contradiction.
export const END_TOLERANCE = 5;

/// What a blind shows (KNX: 0 % is at the top, open; 100 % at the bottom, closed).
///
/// - With a known `position` (5.001) the position, {kind: "position", percent}; the end positions
///   only cross-check it: `conflict` is true where a reported end contradicts it (at the top while
///   the position is more than END_TOLERANCE below it, at the bottom while it is more than
///   END_TOLERANCE above it, or both ends at once).
/// - Without one, the end positions (1 bit each): "open" (at the top), "closed" (at the bottom),
///   "between" (neither: stopped on the way); with only one of them bound, its false value says
///   "notOpen" or "notClosed", never more. Both ends at once is {kind: "unknown", conflict: true}.
/// - Otherwise {kind: "unknown"}. Never a percentage without a position.
export function blindState(position, atTop, atBottom) {
	const both = atTop === true && atBottom === true;
	if (typeof position === "number") {
		const conflict = both || (atTop === true && position > END_TOLERANCE) ||
			(atBottom === true && position < 100 - END_TOLERANCE);
		return { kind: "position", percent: position, conflict };
	}
	if (both) {
		return { kind: "unknown", conflict: true };
	}
	if (atTop === true) {
		return { kind: "open", conflict: false };
	}
	if (atBottom === true) {
		return { kind: "closed", conflict: false };
	}
	if (atTop === false && atBottom === false) {
		return { kind: "between", conflict: false };
	}
	if (atTop === false) {
		return { kind: "notOpen", conflict: false };
	}
	if (atBottom === false) {
		return { kind: "notClosed", conflict: false };
	}
	return { kind: "unknown", conflict: false };
}

/// Whether the room is heated: the `heating` role's value, a boolean as it is, a number (an actuating
/// value in percent, 1.6) while above 0; without a value of `heating` the heating level's, above 0;
/// null when neither is known.
export function heating(value, level) {
	if (typeof value === "boolean") {
		return value;
	}
	if (typeof value === "number") {
		return value > 0;
	}
	return typeof level === "number" ? level > 0 : null;
}

/// The flame steps of a heating level in percent: 0 at 0 % (or below), 1 up to a third, 2 up to two
/// thirds, 3 above; null for an unknown level.
export function flames(level) {
	if (typeof level !== "number") {
		return null;
	}
	if (level <= 0) {
		return 0;
	}
	return level <= 100 / 3 ? 1 : level <= 200 / 3 ? 2 : 3;
}

/// Whether a dimmer is on: its `state`, else the switching `command`'s last value, else its level
/// above 0; null when nothing is known.
export function dimmerOn(state, command, level) {
	if (typeof state === "boolean") {
		return state;
	}
	if (typeof level === "number") {
		return level > 0;
	}
	return typeof command === "boolean" ? command : null;
}
