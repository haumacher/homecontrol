/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

//! The spike's PI room controller in Rust (issue #59): the algorithm of
//! blocks/c/room_controller.c, operation for operation, so both give bit-identical outputs.

#![no_std]

#[link(wasm_import_module = "hc")]
extern "C" {
	fn now() -> i64;
	fn read(port: i32) -> f64;
	fn quality(port: i32) -> i32;
	fn param(id: i32) -> f64;
	fn write(port: i32, value: f64);
	fn log(text: *const u8, length: i32);
}

const MAX_INSTANCES: usize = 8;
const ABI_VERSION: i32 = 1;
const GOOD: i32 = 0;

const IN_TEMPERATURE: i32 = 0;
const IN_SETPOINT: i32 = 1;
const IN_ENABLE: i32 = 2;
const OUT_VALVE: i32 = 0;
const P_XP: i32 = 0;
const P_TN: i32 = 1;
const P_FILTER: i32 = 2;
const P_FALLBACK: i32 = 3;

#[repr(C)]
#[derive(Clone, Copy)]
struct State {
	integral: f64,
	filtered: f64,
	last: i64,
	initialized: i32,
	pad: i32,
}

const ZERO: State = State { integral: 0.0, filtered: 0.0, last: 0, initialized: 0, pad: 0 };

static mut STATES: [State; MAX_INSTANCES] = [ZERO; MAX_INSTANCES];

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
	core::arch::wasm32::unreachable()
}

fn instance(this: i32) -> &'static mut State {
	// An out-of-range instance number is the host's error: trap (the panic handler).
	unsafe { &mut (*core::ptr::addr_of_mut!(STATES))[this as usize] }
}

#[no_mangle]
pub extern "C" fn hc_abi() -> i32 {
	ABI_VERSION
}

#[no_mangle]
pub extern "C" fn hc_instances() -> i32 {
	MAX_INSTANCES as i32
}

#[no_mangle]
pub extern "C" fn hc_state(this: i32) -> i32 {
	instance(this) as *mut State as i32
}

#[no_mangle]
pub extern "C" fn hc_state_size() -> i32 {
	core::mem::size_of::<State>() as i32
}

fn clamp(v: f64, lo: f64, hi: f64) -> f64 {
	if v < lo {
		lo
	} else if v > hi {
		hi
	} else {
		v
	}
}

fn step(s: &mut State) {
	unsafe {
		let t_now = now();
		let temperature = read(IN_TEMPERATURE);
		let have_temperature = quality(IN_TEMPERATURE) == GOOD && temperature == temperature;
		if !have_temperature {
			write(OUT_VALVE, param(P_FALLBACK));
			return;
		}
		if s.initialized == 0 {
			s.filtered = temperature;
			s.last = t_now;
			s.initialized = 1;
		}
		let dt = (t_now - s.last) as f64 / 1000.0;
		s.last = t_now;

		let tau = param(P_FILTER) * 60.0;
		let alpha = if tau > 0.0 { dt / (tau + dt) } else { 1.0 };
		s.filtered += (temperature - s.filtered) * alpha;

		if read(IN_ENABLE) < 0.5 {
			write(OUT_VALVE, 0.0);
			return;
		}

		let kp = 100.0 / param(P_XP);
		let error = read(IN_SETPOINT) - s.filtered;
		let p = kp * error;
		let di = kp * error * dt / (param(P_TN) * 60.0);
		let u = p + s.integral + di;
		if !((u > 100.0 && di > 0.0) || (u < 0.0 && di < 0.0)) {
			s.integral += di;
		}
		write(OUT_VALVE, clamp(p + s.integral, 0.0, 100.0));
	}
}

#[no_mangle]
pub extern "C" fn hc_start(this: i32, _reason: i32, restored: i32) {
	let s = instance(this);
	if restored == 0 {
		s.integral = 0.0;
		s.filtered = 0.0;
		s.initialized = 0;
	}
	unsafe {
		s.last = now();
		let text = b"room controller started (rust)";
		log(text.as_ptr(), text.len() as i32);
	}
}

#[no_mangle]
pub extern "C" fn hc_input(this: i32, _port: i32) {
	step(instance(this));
}

#[no_mangle]
pub extern "C" fn hc_period(this: i32) {
	step(instance(this));
}
