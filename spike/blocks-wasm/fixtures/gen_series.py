#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Writes series.csv, the input series of the spike's determinism test (issue #59, question 8): one
# day of a room as the room controller block sees it. Generated once with a fixed seed and kept;
# the device, the host (WAMR) and Java (Chicory) replay the same rows and must produce the same
# bits (expected.csv, written by the host test with --write-expected).
#
# Columns: t (monotonic ms), call (input port number, or "p" for the period call), temperature
# (°C, quantized to 0.01 like DPT 9.001; "nan" for no value), quality (0 good, 1 stale, 2 unknown),
# setpoint (°C), enable (0/1).
#
# Shape: reports every 30..180 s with jitter, some missing and some doubled (the block integrates
# over time, not per report); a period call every 60 s; a setpoint step 19 -> 21 °C, a window open
# for 2 h (the temperature falls, the valve saturates: anti-windup), the controller disabled for
# 30 min, a sensor outage of 20 min (stale, then no value), night setback.

import math
import random
import sys

random.seed(59)

DAY = 24 * 3600 * 1000
rows = []
temperature = 18.6
heat = 0.0
t_report = 0
t_period = 0


def setpoint_at(t):
	h = t / 3600000.0
	if h < 0.5:
		return 19.0
	if h < 22.0:
		return 21.0
	return 17.5


def enabled_at(t):
	h = t / 3600000.0
	return 0 if 13.0 <= h < 13.5 else 1


def window_at(t):
	h = t / 3600000.0
	return 8.0 <= h < 10.0


def sensor_at(t):
	h = t / 3600000.0
	if 16.0 <= h < 16.2:
		return 1  # stale
	if 16.2 <= h < 16.33:
		return 2  # unknown
	return 0


t = 0
step = 5000
next_report = 0
next_period = 0
while t < DAY:
	# A crude room: heats towards the setpoint with lag, loses heat to 5 °C outside, much faster
	# with the window open; plus sensor noise.
	target = setpoint_at(t)
	heat += ((1.0 if temperature < target else 0.0) - heat) * 0.002
	loss = (temperature - 5.0) * (0.0006 if window_at(t) else 0.00004)
	temperature += heat * 0.004 - loss
	if t >= next_report:
		q = sensor_at(t)
		measured = round(temperature + random.gauss(0.0, 0.03), 2)
		value = "nan" if q == 2 else f"{measured:.2f}"
		tr = max(0, t - random.randint(0, step - 1))  # off the simulation's 5 s grid
		if random.random() >= 0.08:  # 8 % of the reports are lost
			rows.append((tr, "0", value, q, setpoint_at(t), enabled_at(t)))
			if random.random() < 0.05:  # 5 % arrive twice, 0..2 ms apart
				rows.append((tr + random.randint(0, 2), "0", value, q, setpoint_at(t), enabled_at(t)))
		next_report = t + random.randint(30, 180) * 1000 + random.randint(-400, 400)
	if t >= next_period:
		q = sensor_at(t)
		value = "nan" if q == 2 else f"{round(temperature, 2):.2f}"
		rows.append((t, "p", value, q, setpoint_at(t), enabled_at(t)))
		next_period = t + 60000
	t += step

# Setpoint and enable changes arrive as input calls on their own ports.
extra = []
for i in range(1, len(rows)):
	if rows[i][4] != rows[i - 1][4]:
		extra.append((rows[i][0], "1") + rows[i][2:])
	if rows[i][5] != rows[i - 1][5]:
		extra.append((rows[i][0], "2") + rows[i][2:])
rows = sorted(rows + extra, key=lambda r: r[0])

out = open(sys.argv[1] if len(sys.argv) > 1 else "series.csv", "w")
out.write("t,call,temperature,quality,setpoint,enable\n")
for r in rows:
	out.write(f"{r[0]},{r[1]},{r[2]},{r[3]},{r[4]},{r[5]}\n")
out.close()
print(f"{len(rows)} rows", file=sys.stderr)
