/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/demo.h"

#include <cmath>

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.demo";

constexpr uint8_t kSwitch = DatapointFlags::kWritable | DatapointFlags::kTransmit | DatapointFlags::kReadable;
constexpr uint8_t kSensor = DatapointFlags::kReadable;

/// Ids are the indices.
enum : uint16_t {
	kLivingLight,
	kLivingDimmer,
	kLivingTemperature,
	kKitchenLight,
	kKitchenWindow,
	kSetpoint,
	kBrightness,
	kMessage,
	kCount,
};

const DatapointSpec kSpecs[kCount] = {
	{"Living room light", ValueType::boolean, kSwitch},
	{"Living room dimmer", ValueType::unsigned_int, kSwitch},
	{"Living room temperature", ValueType::real, kSensor},
	{"Kitchen light", ValueType::boolean, kSwitch},
	{"Kitchen window", ValueType::boolean, kSensor},
	{"Heating setpoint", ValueType::real, DatapointFlags::kWritable},
	{"Outside brightness", ValueType::real, kSensor},
	{"Status message", ValueType::string, kSensor},
};

} // namespace

DemoTechnology::DemoTechnology(DatapointModel& model, EventLoop& loop, const Clock& clock)
	: model_(model), loop_(loop), clock_(clock) {}

bool DemoTechnology::load() {
	if (loaded_ || model_.size() > 0) {
		Log::error(kTag, "demo datapoints refused: the model holds %u datapoint(s) already",
			static_cast<unsigned>(model_.size()));
		return false;
	}
	if (model_.load(kSpecs, kCount) != DatapointStatus::ok) {
		return false;
	}
	for (uint16_t id = 0; id < kCount; ++id) {
		if (kSpecs[id].flags & DatapointFlags::kReadable) {
			model_.bind(id, *this);
		}
	}
	model_.command(kSetpoint, Value::of_real(21.0));
	loaded_ = true;
	report();
	loop_.schedule_periodic(clock_.monotonic_ms() + kReportMs, kReportMs,
		Callback::member<DemoTechnology, &DemoTechnology::report>(this));
	Log::info(kTag, "%u demo datapoints loaded (simulated values every %u s)", static_cast<unsigned>(kCount),
		static_cast<unsigned>(kReportMs / 1000));
	return true;
}

bool DemoTechnology::command(const Datapoint& datapoint, const Value& value) {
	char text[40];
	Log::info(kTag, "command %s = %s", datapoint.name(), value.format(text, sizeof(text)));
	if (datapoint.id() == kLivingDimmer && value.as_unsigned() > 100) {
		Log::warn(kTag, "refused: the dimmer takes 0..100 %%");
		return false;
	}
	return true;
}

void DemoTechnology::report() {
	++ticks_;
	double minutes = static_cast<double>(clock_.monotonic_ms()) / 60000.0;
	double temperature = std::round((20.5 + 1.5 * std::sin(minutes / 3.0)) * 10.0) / 10.0;
	model_.report(*this, kLivingTemperature, Value::of_real(temperature));
	model_.report(*this, kBrightness, Value::of_real(std::round(800.0 + 600.0 * std::sin(minutes / 7.0))));
	if (ticks_ == 1 || ticks_ % (kWindowMs / kReportMs) == 0) {
		bool open = (ticks_ / (kWindowMs / kReportMs)) % 2 == 1;
		model_.report(*this, kKitchenWindow, Value::of_bool(open));
	}
	if (ticks_ == 1) {
		model_.report(*this, kMessage, *Value::of_string("demo running"));
		model_.report(*this, kLivingLight, Value::of_bool(false));
		model_.report(*this, kKitchenLight, Value::of_bool(true));
		model_.report(*this, kLivingDimmer, Value::of_unsigned(40));
	}
}

} // namespace hc
