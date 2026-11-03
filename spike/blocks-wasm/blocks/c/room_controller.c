/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * The spike's test block (issue #59): a PI room controller against the draft ABI of
 * abi/hc_block.h. Freestanding C, built with clang --target=wasm32 -nostdlib (build-blocks.sh).
 *
 * Ports: in 0 temperature (°C), in 1 setpoint (°C), in 2 enable (0/1); out 0 valve (0..100 %).
 * Parameters: 0 xp (P band, K), 1 tn (reset time, min), 2 filter (time constant of the actual
 * value, min), 3 fallback (valve position without a temperature, %).
 *
 * - The actual value is filtered (first order) over the monotonic time between calls.
 * - P on the error of the filtered value, I integrated over dt (time-based, not per report).
 * - Anti-windup by conditional integration: the integral is frozen while the output is saturated
 *   in the direction the error would push it; it is never reset.
 * - Disabled: the valve is 0 and the integral frozen; the filter keeps following.
 */

#include "../../abi/hc_block.h"

#define MAX_INSTANCES 8

enum { IN_TEMPERATURE = 0, IN_SETPOINT = 1, IN_ENABLE = 2 };
enum { OUT_VALVE = 0 };
enum { P_XP = 0, P_TN = 1, P_FILTER = 2, P_FALLBACK = 3 };

/* The persistent state of one instance (saved and restored by the host as bytes). */
struct State {
	double integral; /* % */
	double filtered; /* °C */
	int64_t last;    /* ms, monotonic */
	int32_t initialized;
	int32_t pad;
};

static struct State states[MAX_INSTANCES];

HC_EXPORT("hc_abi") int32_t abi(void)
{
	return HC_ABI_VERSION;
}

HC_EXPORT("hc_instances") int32_t instances(void)
{
	return MAX_INSTANCES;
}

HC_EXPORT("hc_state") int32_t state(int32_t self)
{
	return (int32_t)(uint32_t)&states[self];
}

HC_EXPORT("hc_state_size") int32_t state_size(void)
{
	return (int32_t)sizeof(struct State);
}

static double clamp(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void step(struct State *s)
{
	int64_t now = hc_now();
	double temperature = hc_read(IN_TEMPERATURE);
	int have_temperature = hc_quality(IN_TEMPERATURE) == HC_GOOD && temperature == temperature;

	if (!have_temperature) {
		hc_write(OUT_VALVE, hc_param(P_FALLBACK));
		return;
	}
	if (!s->initialized) {
		s->filtered = temperature;
		s->last = now;
		s->initialized = 1;
	}
	double dt = (double)(now - s->last) / 1000.0;
	s->last = now;

	double tau = hc_param(P_FILTER) * 60.0;
	double alpha = tau > 0.0 ? dt / (tau + dt) : 1.0;
	s->filtered += (temperature - s->filtered) * alpha;

	if (hc_read(IN_ENABLE) < 0.5) {
		hc_write(OUT_VALVE, 0.0);
		return;
	}

	double kp = 100.0 / hc_param(P_XP);
	double error = hc_read(IN_SETPOINT) - s->filtered;
	double p = kp * error;
	double di = kp * error * dt / (hc_param(P_TN) * 60.0);
	double u = p + s->integral + di;
	if (!((u > 100.0 && di > 0.0) || (u < 0.0 && di < 0.0)))
		s->integral += di;
	hc_write(OUT_VALVE, clamp(p + s->integral, 0.0, 100.0));
}

HC_EXPORT("hc_start") void start(int32_t self, int32_t reason, int32_t restored)
{
	(void)reason;
	struct State *s = &states[self];
	if (!restored) {
		s->integral = 0.0;
		s->filtered = 0.0;
		s->initialized = 0;
	}
	/* The monotonic clock restarts with the controller: integrate from now on. */
	s->last = hc_now();
	hc_log("room controller started");
}

HC_EXPORT("hc_input") void input(int32_t self, int32_t port)
{
	(void)port;
	step(&states[self]);
}

HC_EXPORT("hc_period") void period(int32_t self)
{
	step(&states[self]);
}
