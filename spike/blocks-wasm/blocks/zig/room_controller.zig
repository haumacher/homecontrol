//
// Copyright (c) 2026 Bernhard Haumacher
//
// The spike's PI room controller in Zig (issue #59, question 7): the algorithm of
// blocks/c/room_controller.c, operation for operation, against the draft ABI; wasm32-freestanding,
// no WASI, no allocator, no standard library at run time.

extern "hc" fn now() i64;
extern "hc" fn read(port: i32) f64;
extern "hc" fn quality(port: i32) i32;
extern "hc" fn param(id: i32) f64;
extern "hc" fn write(port: i32, value: f64) void;
extern "hc" fn log(text: [*]const u8, length: i32) void;

const max_instances = 8;
const good: i32 = 0;

const State = extern struct {
	integral: f64 = 0.0,
	filtered: f64 = 0.0,
	last: i64 = 0,
	initialized: i32 = 0,
	pad: i32 = 0,
};

var states: [max_instances]State = @splat(.{});

export fn hc_abi() i32 {
	return 1;
}

export fn hc_instances() i32 {
	return max_instances;
}

export fn hc_state(this: i32) i32 {
	return @intCast(@intFromPtr(&states[@intCast(this)]));
}

export fn hc_state_size() i32 {
	return @sizeOf(State);
}

fn clamp(v: f64, lo: f64, hi: f64) f64 {
	return if (v < lo) lo else if (v > hi) hi else v;
}

fn step(s: *State) void {
	const t = now();
	const temperature = read(0);
	const have_temperature = quality(0) == good and temperature == temperature;
	if (!have_temperature) {
		write(0, param(3));
		return;
	}
	if (s.initialized == 0) {
		s.filtered = temperature;
		s.last = t;
		s.initialized = 1;
	}
	const dt = @as(f64, @floatFromInt(t - s.last)) / 1000.0;
	s.last = t;

	const tau = param(2) * 60.0;
	const alpha = if (tau > 0.0) dt / (tau + dt) else 1.0;
	s.filtered += (temperature - s.filtered) * alpha;

	if (read(2) < 0.5) {
		write(0, 0.0);
		return;
	}

	const kp = 100.0 / param(0);
	const err = read(1) - s.filtered;
	const p = kp * err;
	const di = kp * err * dt / (param(1) * 60.0);
	const u = p + s.integral + di;
	if (!((u > 100.0 and di > 0.0) or (u < 0.0 and di < 0.0)))
		s.integral += di;
	write(0, clamp(p + s.integral, 0.0, 100.0));
}

export fn hc_start(this: i32, reason: i32, restored: i32) void {
	_ = reason;
	const s = &states[@intCast(this)];
	if (restored == 0) {
		s.integral = 0.0;
		s.filtered = 0.0;
		s.initialized = 0;
	}
	s.last = now();
	const text = "room controller started (zig)";
	log(text.ptr, text.len);
}

export fn hc_input(this: i32, port: i32) void {
	_ = port;
	step(&states[@intCast(this)]);
}

export fn hc_period(this: i32) void {
	step(&states[@intCast(this)]);
}
