/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/zigbee/adapter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"

namespace hc {
namespace zigbee {

namespace {

constexpr const char* kTag = "hc.zigbee";

/// Bindings set up per device after the interview (one per endpoint and cluster).
constexpr size_t kMaxBindings = 16;

/// Attributes per Read Attributes request of the refresh.
constexpr size_t kMaxReadAttributes = 8;

constexpr size_t kNameSize = Datapoint::kMaxName + 1;

/// Zigbee's Update Device status (the trust center's view of a join, rejoin or leave).
const char* update_status_name(uint8_t status) {
	switch (status) {
		case 0x00: return "secured rejoin";
		case 0x01: return "unsecured join";
		case 0x02: return "left";
		case 0x03: return "trust center rejoin";
		default: return "unknown update";
	}
}

/// What the trust center decided about an Update Device.
const char* tc_action_name(uint8_t action) {
	switch (action) {
		case 0x00: return "authorize";
		case 0x01: return "deny";
		case 0x02: return "ignore";
		default: return "unknown";
	}
}

/// The stack's authorization type of a joining device (esp_zb_zdo_signal_device_authorized_params_t).
const char* authorization_name(uint8_t type) {
	switch (type) {
		case 0x00: return "legacy device";
		case 0x01: return "trust center link key exchange";
		case 0x02: return "CBKE";
		default: return "unknown kind";
	}
}

/// The outcome of an authorization: 0 success; for the link key exchange 1 a timeout, otherwise a
/// failure.
const char* authorization_status_name(uint8_t type, uint8_t status) {
	if (status == 0) {
		return "success";
	}
	return type == 0x01 && status == 0x01 ? "timeout" : "failed";
}

void fail(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
}

bool same_point(const ZigbeePoint& a, const ZigbeePoint& b) {
	return a.ieee == b.ieee && a.endpoint == b.endpoint && a.function == b.function;
}

/// The same datapoint: point, command cluster and (for the generic name) zone type.
bool same_mapping(const ZigbeePoint& a, const ZigbeePoint& b) {
	return same_point(a, b) && a.command_cluster == b.command_cluster && a.zone_type == b.zone_type;
}

/// The unit a function's value is in, for the UI (nullptr: none).
const char* unit_of(Function function) {
	switch (function) {
		case Function::level:
		case Function::humidity:
		case Function::battery: return "%";
		case Function::color_temperature: return "K";
		case Function::temperature: return "\xc2\xb0" "C";
		case Function::illuminance: return "lx";
		default: return nullptr;
	}
}

/// The reporting the bindings `table` give the function of the device's endpoint: a bound device's
/// binding of it, or the default of a function the generic rules map for a device without
/// bindings. False when the function is no point of the device (a bound device's unbound function,
/// a function the device does not have).
bool reporting_in(const ZigbeePoint* table, size_t count, const ZigbeeDevice& device, uint8_t endpoint,
	Function function, Reporting& out) {
	bool bound = false;
	for (size_t i = 0; i < count; ++i) {
		const ZigbeePoint& point = table[i];
		if (point.ieee != device.ieee) {
			continue;
		}
		bound = true;
		if (point.endpoint == endpoint && point.function == function) {
			out = point.reporting;
			return true;
		}
	}
	if (bound || !device.has_result()) {
		return false;
	}
	const ZigbeeEndpoint* e = device.endpoint(endpoint);
	const FunctionInfo& info = function_info(function);
	if (e == nullptr || !info.server || !e->has_in(info.cluster)) {
		return false;
	}
	out = default_reporting(function);
	return true;
}

const char* step_name(uint8_t step) {
	switch (step) {
		case 0: return "active endpoints";
		case 1: return "simple descriptor";
		case 2: return "manufacturer and model";
		case 3: return "IAS zone type";
	}
	return "?";
}

static_assert(ZigbeeAdapter::kMaxQueriedEndpoints == ZigbeeDevice::kMaxMissing,
	"the queried endpoints and the missing list of an incomplete interview hold each other");

} // namespace

ZigbeeAdapter::ZigbeeAdapter(EventLoop& loop, const Clock& clock, DatapointModel& model, ZigbeeDriver& driver,
	BlobStore* store)
	: loop_(loop), clock_(clock), model_(model), driver_(driver), database_(store), persistent_(store != nullptr) {}

/// The tables a dynamic_specs() call built: the image's bindings (copied, with their names), the
/// points (bindings first, then the generic ones), the specs and names of the generic points, and
/// room for the ids model_loaded() resolves.
struct ZigbeeAdapter::Pending {
	ZigbeePoint* overrides = nullptr;
	char* override_names = nullptr;
	size_t override_count = 0;
	ZigbeePoint* points = nullptr;
	uint16_t* ids = nullptr;
	size_t count = 0;
	image::DynamicSpec* specs = nullptr;
	char* names = nullptr;

	~Pending() {
		delete[] overrides;
		delete[] override_names;
		delete[] points;
		delete[] ids;
		delete[] specs;
		delete[] names;
	}
};

ZigbeeAdapter::~ZigbeeAdapter() {
	loop_.cancel(check_timer_);
	loop_.cancel(refresh_timer_);
	loop_.cancel(held_timer_);
	loop_.cancel(interview_.timer);
	loop_.cancel(topology_.timer);
	loop_.cancel(topology_period_);
	loop_.cancel(rejoin_.timer);
	delete[] points_;
	delete[] ids_;
	free_overrides();
	free_pending();
}

void ZigbeeAdapter::free_pending() {
	delete pending_;
	pending_ = nullptr;
}

void ZigbeeAdapter::free_overrides() {
	delete[] overrides_;
	delete[] override_names_;
	overrides_ = nullptr;
	override_names_ = nullptr;
	override_count_ = 0;
}

const char* ZigbeeAdapter::device_text(const ZigbeeDevice& device, char* buffer, size_t size) const {
	char ieee[kIeeeText];
	format_ieee(device.ieee, ieee);
	const char* name = device.label[0] != '\0' ? device.label : device.model;
	if (name[0] != '\0') {
		std::snprintf(buffer, size, "%s (%s)", ieee, name);
	} else {
		std::snprintf(buffer, size, "%s", ieee);
	}
	return buffer;
}

const char* ZigbeeAdapter::node_text(NodeAddress address, char* buffer, size_t size) {
	ZigbeeDevice* device = address.ieee != 0 ? database_.find(address.ieee) : database_.find_short(address.short_addr);
	if (device != nullptr) {
		return device_text(*device, buffer, size);
	}
	if (address.ieee != 0) {
		format_ieee(address.ieee, buffer); // kIeeeText <= size
		return buffer;
	}
	std::snprintf(buffer, size, "an unknown device");
	return buffer;
}

const char* ZigbeeAdapter::parent_text(uint16_t parent_short, char* buffer, size_t size) {
	if (parent_short == 0x0000) {
		std::snprintf(buffer, size, "the coordinator");
		return buffer;
	}
	ZigbeeDevice* parent = database_.find_short(parent_short);
	if (parent != nullptr) {
		return device_text(*parent, buffer, size);
	}
	std::snprintf(buffer, size, "router 0x%04x", static_cast<unsigned>(parent_short));
	return buffer;
}

// ---- Start and configuration ----------------------------------------------------------------

bool ZigbeeAdapter::start(const char** error) {
	if (started_) {
		fail(error, "the Zigbee adapter is started already");
		return false;
	}
	started_ = true;
	start_ms_ = clock_.monotonic_ms();
	if (!persistent_) {
		Log::warn(kTag, "no blob store: the device database is kept in memory only (lost at a restart)");
	}
	const char* why = nullptr;
	if (!database_.load(&why)) {
		Log::error(kTag, "device database %s unreadable (%s): starting empty; devices are interviewed again when heard, "
			"the blob is replaced with the next write", DeviceDatabase::kBlobName, why != nullptr ? why : "?");
	} else {
		Log::info(kTag, "%u device(s) in the database", static_cast<unsigned>(database_.size()));
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		database_.at(i).refresh = database_.at(i).has_result();
		database_.at(i).saved_seen_s = database_.at(i).last_seen_s;
	}
	check_timer_ = loop_.schedule_periodic(start_ms_ + kCheckPeriodMs, kCheckPeriodMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::check_stale>(this, "zigbee.check_stale"));
	topology_period_ = loop_.schedule_periodic(start_ms_ + kTopologyPeriodMs, kTopologyPeriodMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::topology_tick>(this, "zigbee.topology_tick"));
	bool ok = rebuild(error);
	schedule_refresh();
	return ok;
}

bool ZigbeeAdapter::configure(const ZigbeeOverride* overrides, size_t count, const char** error) {
	if (has_spec_listener()) {
		Log::error(kTag, "bindings refused: the image service hands them over with the model (dynamic datapoints)");
		fail(error, "the image service hands the Zigbee bindings over");
		return false;
	}
	if (count > kMaxOverrides) {
		Log::error(kTag, "bindings refused: %u exceed the capacity of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxOverrides));
		fail(error, "too many Zigbee bindings");
		return false;
	}
	if (overrides == nullptr && count > 0) {
		fail(error, "bindings missing");
		return false;
	}
	size_t names_bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		const char* why = nullptr;
		std::optional<ZigbeePoint> point = override_point(overrides[i], &why);
		if (!point) {
			Log::error(kTag, "bindings refused: binding %u ('%s'): %s", static_cast<unsigned>(i),
				overrides[i].name != nullptr ? overrides[i].name : "", why);
			fail(error, why);
			return false;
		}
		size_t length = std::strlen(overrides[i].name);
		if (length > Datapoint::kMaxName) {
			Log::error(kTag, "bindings refused: binding %u: name longer than %u bytes", static_cast<unsigned>(i),
				static_cast<unsigned>(Datapoint::kMaxName));
			fail(error, "binding name too long");
			return false;
		}
		for (size_t j = 0; j < i; ++j) {
			std::optional<ZigbeePoint> other = override_point(overrides[j]);
			if (std::strcmp(overrides[i].name, overrides[j].name) == 0) {
				Log::error(kTag, "bindings refused: the name '%s' is bound twice", overrides[i].name);
				fail(error, "a datapoint name is bound twice");
				return false;
			}
			if (other && same_point(*point, *other)) {
				Log::error(kTag, "bindings refused: '%s' and '%s' bind the same function of the same endpoint",
					overrides[j].name, overrides[i].name);
				fail(error, "a function of an endpoint is bound twice");
				return false;
			}
		}
		names_bytes += length + 1;
	}
	ZigbeePoint* table = count > 0 ? new (std::nothrow) ZigbeePoint[count] : nullptr;
	char* names = names_bytes > 0 ? new (std::nothrow) char[names_bytes] : nullptr;
	if ((count > 0 && table == nullptr) || (names_bytes > 0 && names == nullptr)) {
		delete[] table;
		delete[] names;
		Log::error(kTag, "bindings refused: out of memory");
		fail(error, "out of memory");
		return false;
	}
	char* next = names;
	for (size_t i = 0; i < count; ++i) {
		table[i] = *override_point(overrides[i]);
		size_t length = std::strlen(overrides[i].name);
		std::memcpy(next, overrides[i].name, length + 1);
		table[i].name = next;
		next += length + 1;
	}
	ZigbeePoint* old_table = overrides_;
	char* old_names = override_names_;
	size_t old_count = override_count_;
	overrides_ = table;
	override_names_ = names;
	override_count_ = count;
	if (started_ && !rebuild(error)) {
		overrides_ = old_table;
		override_names_ = old_names;
		override_count_ = old_count;
		delete[] table;
		delete[] names;
		Log::error(kTag, "bindings refused: their datapoints cannot be loaded; the previous bindings stay");
		return false;
	}
	Log::info(kTag, "%u binding(s) from the runtime image", static_cast<unsigned>(count));
	if (started_ && bindings_known_) {
		reconfigure_reporting(old_table, old_count);
	}
	bindings_known_ = true;
	delete[] old_table;
	delete[] old_names;
	return true;
}

bool ZigbeeAdapter::device_has_overrides(Ieee ieee) const {
	for (size_t i = 0; i < override_count_; ++i) {
		if (overrides_[i].ieee == ieee) {
			return true;
		}
	}
	return false;
}

bool ZigbeeAdapter::specs_differ() const {
	if (!loaded_) {
		return true;
	}
	size_t generic = point_count_ - override_count_;
	size_t count = 0;
	ZigbeePoint mapped[ZigbeeDevice::kMaxEndpoints * (kFunctionCount + 1)];
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (device_has_overrides(device.ieee)) {
			continue;
		}
		size_t capacity = sizeof(mapped) / sizeof(mapped[0]);
		size_t n = map_device(device, mapped, capacity);
		if (n > capacity) {
			return true; // cannot compare: reload
		}
		for (size_t k = 0; k < n; ++k) {
			if (count >= generic || !same_mapping(mapped[k], points_[override_count_ + count])) {
				return true;
			}
			++count;
		}
	}
	return count != generic;
}

bool ZigbeeAdapter::rebuild(const char** error) {
	if (has_spec_listener()) {
		// The image service composes the model: tell it when the generic datapoints change.
		if (specs_differ()) {
			notify_specs_changed();
		}
		return true;
	}
	for (size_t id = 0; id < model_.size(); ++id) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(id));
		if (dp->technology() != this) {
			Log::error(kTag, "datapoints not loaded: the model holds '%s', which is not Zigbee's (until runtime images "
				"exist, issue #8, Zigbee loads the model alone)", dp->name());
			fail(error, "the model holds datapoints of another owner");
			loaded_ = false;
			return false;
		}
	}
	size_t count = override_count_;
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (!device_has_overrides(device.ieee)) {
			count += map_device(device, nullptr, 0);
		}
	}
	ZigbeePoint* table = count > 0 ? new (std::nothrow) ZigbeePoint[count] : nullptr;
	uint16_t* ids = count > 0 ? new (std::nothrow) uint16_t[count] : nullptr;
	DatapointSpec* specs = count > 0 ? new (std::nothrow) DatapointSpec[count] : nullptr;
	char* names = count > 0 ? new (std::nothrow) char[count * kNameSize] : nullptr;
	struct Saved {
		Value value;
		Quality quality;
		bool pending; ///< A command held while the network is not up (homecontrol #72).
	};
	Saved* saved = loaded_ && point_count_ > 0 ? new (std::nothrow) Saved[point_count_] : nullptr;
	if ((count > 0 && (table == nullptr || ids == nullptr || specs == nullptr || names == nullptr)) ||
		(loaded_ && point_count_ > 0 && saved == nullptr)) {
		delete[] table;
		delete[] ids;
		delete[] specs;
		delete[] names;
		delete[] saved;
		Log::error(kTag, "datapoints not loaded: out of memory (%u datapoints)", static_cast<unsigned>(count));
		fail(error, "out of memory");
		return false;
	}
	size_t filled = 0;
	for (size_t i = 0; i < override_count_; ++i) {
		table[filled++] = overrides_[i];
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (!device_has_overrides(device.ieee)) {
			filled += map_device(device, table + filled, count - filled);
		}
	}
	for (size_t i = 0; i < count; ++i) {
		char* name = names + i * kNameSize;
		point_name(table[i], name, kNameSize);
		specs[i] = DatapointSpec{name, table[i].type(), table[i].flags()};
		table[i].name = nullptr; // the model keeps the name
	}
	if (saved != nullptr) {
		for (size_t i = 0; i < point_count_; ++i) {
			const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
			saved[i] = dp != nullptr ? Saved{dp->value(), dp->quality(), dp->pending()}
			                         : Saved{Value(), Quality::unknown, false};
		}
	}
	DatapointStatus status = model_.load(specs, count);
	delete[] specs;
	delete[] names;
	if (status != DatapointStatus::ok) {
		delete[] table;
		delete[] ids;
		delete[] saved;
		Log::error(kTag, "datapoints not loaded: the model refused them (%s)", datapoint_status_name(status));
		fail(error, "the model refused the Zigbee datapoints");
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		model_.bind(static_cast<uint16_t>(i), *this);
		ids[i] = static_cast<uint16_t>(i);
	}
	// Carry the values over: a good value is reported again, a stale one carried as stale (the
	// device is still silent; a restore would make it good).
	if (saved != nullptr) {
		for (size_t i = 0; i < count; ++i) {
			for (size_t j = 0; j < point_count_; ++j) {
				if (!same_point(table[i], points_[j]) || saved[j].quality == Quality::unknown) {
					continue;
				}
				if (saved[j].quality == Quality::good) {
					model_.report(*this, static_cast<uint16_t>(i), saved[j].value);
				} else {
					std::optional<Value> value = saved[j].value.convert(model_.get(static_cast<uint16_t>(i))->type());
					if (value) {
						model_.carry(static_cast<uint16_t>(i), *value, Quality::stale, 0);
					}
				}
				if (saved[j].pending) {
					model_.set_pending(*this, static_cast<uint16_t>(i), true);
				}
				break;
			}
		}
	}
	delete[] saved;
	delete[] points_;
	delete[] ids_;
	points_ = table;
	ids_ = ids;
	point_count_ = count;
	loaded_ = true;
	Log::info(kTag, "%u datapoint(s) loaded", static_cast<unsigned>(count));
	return true;
}

// ---- With the image service (dynamic datapoints) ----------------------------------------------

bool ZigbeeAdapter::dynamic_specs(const image::ImageBindings& bindings, const image::DynamicSpec*& specs,
	size_t& count, const char** error) {
	free_pending();
	if (bindings.zigbee_count > kMaxOverrides || (bindings.zigbee == nullptr && bindings.zigbee_count > 0)) {
		fail(error, "too many Zigbee bindings");
		return false;
	}
	Pending* next = new (std::nothrow) Pending();
	if (next == nullptr) {
		fail(error, "out of memory");
		return false;
	}
	// The image's bindings (the loader checked them; override_point() cannot refuse them here).
	size_t names_bytes = 0;
	for (size_t i = 0; i < bindings.zigbee_count; ++i) {
		names_bytes += std::strlen(bindings.zigbee[i].name) + 1;
	}
	next->override_count = bindings.zigbee_count;
	if (next->override_count > 0) {
		next->overrides = new (std::nothrow) ZigbeePoint[next->override_count];
		next->override_names = new (std::nothrow) char[names_bytes];
		if (next->overrides == nullptr || next->override_names == nullptr) {
			delete next;
			fail(error, "out of memory");
			return false;
		}
	}
	char* cursor = next->override_names;
	for (size_t i = 0; i < next->override_count; ++i) {
		const char* why = nullptr;
		std::optional<ZigbeePoint> point = override_point(bindings.zigbee[i], &why);
		if (!point) {
			Log::error(kTag, "binding of '%s' refused: %s", bindings.zigbee[i].name != nullptr ? bindings.zigbee[i].name : "",
				why);
			delete next;
			fail(error, why);
			return false;
		}
		size_t length = std::strlen(bindings.zigbee[i].name);
		std::memcpy(cursor, bindings.zigbee[i].name, length + 1);
		next->overrides[i] = *point;
		next->overrides[i].name = cursor;
		cursor += length + 1;
	}
	auto bound = [next](Ieee ieee) {
		for (size_t i = 0; i < next->override_count; ++i) {
			if (next->overrides[i].ieee == ieee) {
				return true;
			}
		}
		return false;
	};
	// The generic points of the devices the image does not bind.
	size_t generic = 0;
	for (size_t i = 0; i < database_.size(); ++i) {
		if (!bound(database_.at(i).ieee)) {
			generic += map_device(database_.at(i), nullptr, 0);
		}
	}
	next->count = next->override_count + generic;
	if (next->count > 0) {
		next->points = new (std::nothrow) ZigbeePoint[next->count];
		next->ids = new (std::nothrow) uint16_t[next->count];
		if (next->points == nullptr || next->ids == nullptr) {
			delete next;
			fail(error, "out of memory");
			return false;
		}
	}
	if (generic > 0) {
		next->specs = new (std::nothrow) image::DynamicSpec[generic];
		next->names = new (std::nothrow) char[generic * kNameSize];
		if (next->specs == nullptr || next->names == nullptr) {
			delete next;
			fail(error, "out of memory");
			return false;
		}
	}
	for (size_t i = 0; i < next->override_count; ++i) {
		next->points[i] = next->overrides[i];
	}
	size_t filled = next->override_count;
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (!bound(device.ieee)) {
			filled += map_device(device, next->points + filled, next->count - filled);
		}
	}
	for (size_t g = 0; g < generic; ++g) {
		ZigbeePoint& point = next->points[next->override_count + g];
		char* name = next->names + g * kNameSize;
		point.name = nullptr;
		point_name(point, name, kNameSize);
		image::DynamicSpec& spec = next->specs[g];
		spec.spec = DatapointSpec{name, point.type(), point.flags()};
		spec.technology = this;
		spec.unit = unit_of(point.function);
	}
	pending_ = next;
	specs = next->specs;
	count = generic;
	return true;
}

void ZigbeeAdapter::model_loaded(const image::ImageBindings& bindings) {
	(void)bindings; // taken over by dynamic_specs(), which the image service called with the same
	if (pending_ == nullptr) {
		Log::error(kTag, "model loaded without the Zigbee specs asked for: the datapoints stay unbound");
		loaded_ = false;
		return;
	}
	Pending* next = pending_;
	pending_ = nullptr;
	size_t bound = 0;
	size_t missing = 0;
	for (size_t i = 0; i < next->count; ++i) {
		const char* name = i < next->override_count ? next->points[i].name : next->specs[i - next->override_count].spec.name;
		const Datapoint* dp = model_.find(name);
		next->ids[i] = dp != nullptr ? dp->id() : kNoId;
		if (dp == nullptr) {
			++missing;
			Log::error(kTag, "datapoint '%s' is not in the model: its device function stays unmapped", name);
			continue;
		}
		if (dp->technology() == nullptr) {
			model_.bind(dp->id(), *this);
		} else if (dp->technology() != this) {
			Log::error(kTag, "datapoint '%s' belongs to %s: not bound to Zigbee", name, dp->technology()->name());
			next->ids[i] = kNoId;
			continue;
		}
		++bound;
	}
	// Devices with points the model did not have are read (their values may be unknown).
	bool refresh = false;
	for (size_t i = 0; i < next->count; ++i) {
		bool known = false;
		for (size_t j = 0; loaded_ && j < point_count_ && !known; ++j) {
			known = same_point(next->points[i], points_[j]) && ids_[j] != kNoId;
		}
		ZigbeeDevice* device = known ? nullptr : database_.find(next->points[i].ieee);
		if (device != nullptr && device->has_result() && !device->refresh) {
			device->refresh = true;
			refresh = true;
		}
	}
	// Adopt: the bindings, the points and their ids.
	ZigbeePoint* old_overrides = overrides_;
	char* old_names = override_names_;
	size_t old_count = override_count_;
	overrides_ = next->overrides;
	override_names_ = next->override_names;
	override_count_ = next->override_count;
	next->overrides = nullptr;
	next->override_names = nullptr;
	if (started_ && bindings_known_) {
		reconfigure_reporting(old_overrides, old_count);
	}
	bindings_known_ = true;
	delete[] old_overrides;
	delete[] old_names;
	delete[] points_;
	delete[] ids_;
	points_ = next->points;
	ids_ = next->ids;
	point_count_ = next->count;
	next->points = nullptr;
	next->ids = nullptr;
	delete next; // the specs' names: the model keeps its own copies
	loaded_ = true;
	if (refresh && started_) {
		schedule_refresh();
	}
	Log::info(kTag, "%u datapoint(s) bound (%u from the image's bindings)%s", static_cast<unsigned>(bound),
		static_cast<unsigned>(override_count_), missing > 0 ? "; some are missing from the model" : "");
}

void ZigbeeAdapter::model_cleared() {
	free_pending();
	loaded_ = false;
}

size_t ZigbeeAdapter::device_points(const ZigbeeDevice& device, ZigbeePoint* out, size_t capacity) const {
	if (!device_has_overrides(device.ieee)) {
		size_t n = map_device(device, out, capacity);
		return n < capacity ? n : capacity;
	}
	size_t n = 0;
	for (size_t i = 0; i < override_count_ && n < capacity; ++i) {
		if (overrides_[i].ieee == device.ieee && device.endpoint(overrides_[i].endpoint) != nullptr) {
			out[n++] = overrides_[i];
		}
	}
	return n;
}

long ZigbeeAdapter::index_of(uint16_t id) const {
	for (size_t i = 0; loaded_ && i < point_count_; ++i) {
		if (ids_[i] == id) {
			return static_cast<long>(i);
		}
	}
	return -1;
}

bool ZigbeeAdapter::save_database() {
	if (!persistent_) {
		return false;
	}
	const char* why = nullptr;
	if (!database_.save(&why)) {
		Log::error(kTag, "device database not written (%s): the previous one stays", why != nullptr ? why : "?");
		return false;
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		database_.at(i).saved_seen_s = database_.at(i).last_seen_s;
	}
	return true;
}

// ---- Operations -----------------------------------------------------------------------------

bool ZigbeeAdapter::permit_join(uint32_t seconds, const char** error) {
	if (seconds > kMaxPermitJoinS) {
		fail(error, "permit join takes 0..254 seconds");
		return false;
	}
	if (!network_.up) {
		fail(error, "the Zigbee network is not up");
		return false;
	}
	if (!driver_.permit_join(static_cast<uint8_t>(seconds))) {
		fail(error, "the Zigbee stack refused the permit join");
		return false;
	}
	if (seconds > 0) {
		Log::info(kTag, "opening the network for joining for %u s", static_cast<unsigned>(seconds));
	} else {
		Log::info(kTag, "closing the network for joining");
	}
	return true;
}

bool ZigbeeAdapter::remove(Ieee ieee, const char** error) {
	ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		fail(error, "no such device");
		return false;
	}
	char text[64];
	device_text(*device, text, sizeof(text));
	// The trust center's key goes first, whatever the leave does (homecontrol #99): the key restored for
	// a legacy device would otherwise let the removed device back in by a trust-center rejoin.
	if (!driver_.forget_device(ieee)) {
		Log::warn(kTag, "%s not removed: the trust center's link key for it could not be dropped (the stack does not "
			"run or kept it); try again when the network is up", text);
		fail(error, "the trust center's link key for the device could not be dropped (try again when the network is up)");
		return false;
	}
	if (device->short_addr == kNoShortAddress || !driver_.leave(ieee, device->short_addr)) {
		Log::warn(kTag, "%s not asked to leave (no address or the stack refused): it rejoins as a new device "
			"when it is heard again", text);
	}
	if (interview_.active && interview_.ieee == ieee) {
		end_interview();
	}
	if (rejoin_.ieee == ieee) {
		end_rejoin();
	}
	topology_abort(); // the database indices change
	database_.remove(ieee);
	save_database();
	Log::info(kTag, "%s removed", text);
	if (started_) {
		rebuild(nullptr);
	}
	notify(ZigbeeEvent::Kind::removed, ieee);
	start_pending_interview();
	return true;
}

bool ZigbeeAdapter::identify(Ieee ieee, const char** error) {
	ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		fail(error, "no such device");
		return false;
	}
	if (device->endpoint_count == 0 || device->short_addr == kNoShortAddress) {
		fail(error, "the device is not interviewed yet");
		return false;
	}
	uint8_t endpoint = device->endpoints[0].id;
	for (size_t i = 0; i < device->endpoint_count; ++i) {
		if (device->endpoints[i].has_in(cluster::kIdentify)) {
			endpoint = device->endpoints[i].id;
			break;
		}
	}
	if (!driver_.identify(ieee, device->short_addr, endpoint, kIdentifySeconds)) {
		fail(error, "the Zigbee stack refused the identify command");
		return false;
	}
	return true;
}

bool ZigbeeAdapter::set_label(Ieee ieee, const char* label, const char** error) {
	ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		fail(error, "no such device");
		return false;
	}
	if (label == nullptr || std::strlen(label) > ZigbeeDevice::kMaxText) {
		fail(error, "a label has at most 32 bytes");
		return false;
	}
	char previous[sizeof(device->label)];
	std::memcpy(previous, device->label, sizeof(previous));
	copy_text(device->label, sizeof(device->label), label);
	if (persistent_ && !save_database()) {
		std::memcpy(device->label, previous, sizeof(previous));
		fail(error, "the device database could not be written");
		return false;
	}
	notify(ZigbeeEvent::Kind::changed, ieee);
	return true;
}

bool ZigbeeAdapter::interview(Ieee ieee, const char** error) {
	ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		fail(error, "no such device");
		return false;
	}
	if (device->short_addr == kNoShortAddress) {
		fail(error, "the device's network address is unknown");
		return false;
	}
	device->interview_pending = true;
	device->interview_retry = false;
	if (interview_.active) {
		Log::info(kTag, "interview queued behind the running one");
		return true;
	}
	start_pending_interview();
	return true;
}

bool ZigbeeAdapter::request_rejoin(Ieee ieee, const char** error) {
	if (!network_.up) {
		fail(error, "the Zigbee network is not up");
		return false;
	}
	ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		fail(error, "no such device");
		return false;
	}
	if (device->left) {
		fail(error, "the device left the network");
		return false;
	}
	if (device->short_addr == kNoShortAddress) {
		fail(error, "the device's network address is unknown");
		return false;
	}
	if (rejoin_.ieee != 0 && rejoin_.ieee != ieee) {
		fail(error, "the requested rejoin of another device is pending");
		return false;
	}
	if (!driver_.leave_and_rejoin(ieee, device->short_addr)) {
		fail(error, "the Zigbee stack refused the leave request");
		return false;
	}
	// A new request for the same device starts over.
	end_rejoin();
	uint64_t now = clock_.monotonic_ms();
	rejoin_.ieee = ieee;
	rejoin_.short_addr = device->short_addr;
	rejoin_.since_ms = now;
	rejoin_.sent_ms = now;
	rejoin_.sends = 1;
	rejoin_.timer = loop_.schedule_at(now + kRejoinWaitMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::rejoin_expired>(this, "zigbee.rejoin_expired"));
	char text[64];
	Log::info(kTag, "%s (short 0x%04x) asked to leave and rejoin (ZDO Mgmt_Leave with rejoin; a sleepy device takes it "
		"from its parent when it next polls)", device_text(*device, text, sizeof(text)),
		static_cast<unsigned>(device->short_addr));
	notify(ZigbeeEvent::Kind::changed, ieee);
	return true;
}

void ZigbeeAdapter::resend_rejoin(ZigbeeDevice& device) {
	uint64_t now = clock_.monotonic_ms();
	rejoin_.sent_ms = now; // also after a refusal: the next try waits kRejoinResendMs
	char text[64];
	device_text(device, text, sizeof(text));
	if (!driver_.leave_and_rejoin(device.ieee, device.short_addr)) {
		Log::warn(kTag, "%s heard, but the Zigbee stack refused to send the leave request again", text);
		return;
	}
	rejoin_.short_addr = device.short_addr;
	++rejoin_.sends;
	Log::info(kTag, "%s heard: asked again to leave and rejoin (request %u of %u)", text,
		static_cast<unsigned>(rejoin_.sends), static_cast<unsigned>(kRejoinSends));
}

void ZigbeeAdapter::rejoin_returned(const ZigbeeDevice& device) {
	char text[64];
	Log::info(kTag, "%s came back %u s after the requested rejoin (short 0x%04x; requested %u time(s)%s)",
		device_text(device, text, sizeof(text)),
		static_cast<unsigned>((clock_.monotonic_ms() - rejoin_.since_ms) / 1000),
		static_cast<unsigned>(device.short_addr), static_cast<unsigned>(rejoin_.sends),
		rejoin_.accepted ? "" : ", its answer was not heard");
	end_rejoin();
	notify(ZigbeeEvent::Kind::changed, device.ieee);
}

void ZigbeeAdapter::rejoin_expired() {
	rejoin_.timer = TimerHandle();
	Ieee ieee = rejoin_.ieee;
	if (ieee == 0) {
		return;
	}
	const ZigbeeDevice* device = database_.find(ieee);
	char text[64];
	if (device != nullptr) {
		device_text(*device, text, sizeof(text));
	} else {
		format_ieee(ieee, text);
	}
	Log::warn(kTag, "%s did not come back within %u min of the requested rejoin (requested %u time(s); %s)", text,
		static_cast<unsigned>(kRejoinWaitMs / 60000), static_cast<unsigned>(rejoin_.sends),
		rejoin_.accepted ? "it took the request" : "no answer that it took the request");
	end_rejoin();
	notify(ZigbeeEvent::Kind::changed, ieee);
}

void ZigbeeAdapter::end_rejoin() {
	loop_.cancel(rejoin_.timer);
	rejoin_ = Rejoin();
}

// ---- The trust center -----------------------------------------------------------------------

void ZigbeeAdapter::trust_known_devices() {
	if (driver_.set_rejoin_policy(true, false)) {
		Log::info(kTag, "trust center: trust-center rejoins accepted from the devices it holds a link key for, refused "
			"from any other");
	} else {
		Log::warn(kTag, "trust center: the rejoin policy was not set (the stack does not run): its defaults apply");
	}
	size_t known = 0;
	size_t restored = 0;
	size_t refused = 0;
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (device.left) {
			continue;
		}
		++known;
		switch (trust(device, "at the network's start")) {
			case ZigbeeDriver::Trust::restored: ++restored; break;
			case ZigbeeDriver::Trust::refused: ++refused; break;
			case ZigbeeDriver::Trust::kept: break;
		}
	}
	Log::info(kTag, "trust center: holds a link key for %u of %u known device(s) (%u restored)",
		static_cast<unsigned>(known - refused), static_cast<unsigned>(known), static_cast<unsigned>(restored));
}

ZigbeeDriver::Trust ZigbeeAdapter::trust(const ZigbeeDevice& device, const char* why) {
	if (device.left) {
		return ZigbeeDriver::Trust::kept; // a device that left is no longer known to the trust center
	}
	int stack_answer = 0;
	ZigbeeDriver::Trust result = driver_.trust_device(device.ieee, stack_answer);
	char text[64];
	if (result == ZigbeeDriver::Trust::restored) {
		Log::info(kTag, "%s: the trust center holds its link key again %s (the well-known key it joined with): its "
			"trust-center rejoin is accepted", device_text(device, text, sizeof(text)), why);
	} else if (result == ZigbeeDriver::Trust::refused) {
		Log::warn(kTag, "%s: the trust center holds no link key for it and did not take one %s (stack answer %d): its "
			"trust-center rejoin is refused", device_text(device, text, sizeof(text)), why, stack_answer);
	}
	return result;
}

// ---- Device bookkeeping ---------------------------------------------------------------------

ZigbeeDevice* ZigbeeAdapter::resolve(NodeAddress address, bool add) {
	ZigbeeDevice* device = nullptr;
	if (address.ieee != 0) {
		device = database_.find(address.ieee);
		if (device == nullptr && add) {
			const char* why = nullptr;
			device = database_.add(address.ieee, address.short_addr, &why);
			char ieee[kIeeeText];
			format_ieee(address.ieee, ieee);
			if (device == nullptr) {
				Log::error(kTag, "device %s not added: %s", ieee, why);
			} else {
				Log::info(kTag, "new device %s (short 0x%04x)", ieee, static_cast<unsigned>(address.short_addr));
				notify(ZigbeeEvent::Kind::joined, address.ieee);
			}
			return device;
		}
		if (device != nullptr && address.short_addr != kNoShortAddress && device->short_addr != address.short_addr) {
			database_.add(address.ieee, address.short_addr); // moves the short address to this device
		}
	} else {
		device = database_.find_short(address.short_addr);
	}
	return device;
}

void ZigbeeAdapter::touch(ZigbeeDevice& device) {
	uint64_t now = clock_.monotonic_ms();
	device.seen_ms = now;
	WallTime wall = clock_.wall_time();
	if (wall.synchronized) {
		device.last_seen_s = wall.seconds;
	}
	if (device.stale) {
		device.stale = false;
		device.refresh = device.has_result();
		char text[64];
		Log::info(kTag, "%s heard again: reading its datapoints", device_text(device, text, sizeof(text)));
		schedule_refresh();
		notify(ZigbeeEvent::Kind::changed, device.ieee);
	}
	// A requested rejoin the device has not taken is sent again when it is heard (homecontrol #99): a
	// sleepy device polls its parent right after its own frames, and takes the request then.
	if (rejoin_.ieee == device.ieee && !rejoin_.accepted && rejoin_.sends < kRejoinSends &&
		now - rejoin_.sent_ms >= kRejoinResendMs) {
		resend_rejoin(device);
	}
	// A device not interviewed (never, failed or incomplete) is interviewed when it is heard, at most
	// once per kInterviewRetryMs: a sleepy device answers right after its own frames.
	if (!device.interviewed && !device.interview_pending &&
		(device.interview_ms == 0 || now - device.interview_ms >= kInterviewRetryMs) &&
		!(interview_.active && interview_.ieee == device.ieee)) {
		device.interview_pending = true;
		device.interview_retry = device.incomplete();
		start_pending_interview();
	}
}

uint32_t ZigbeeAdapter::stale_timeout_s(Ieee ieee) const {
	const ZigbeeDevice* device = database_.find(ieee);
	if (device == nullptr) {
		return 0;
	}
	// Every frame of the device counts as heard (touch()), so the most frequent promise is the one to
	// check: the shortest periodic maximum (homecontrol #98).
	uint32_t shortest = 0;
	for (size_t i = 0; i < point_count_; ++i) {
		const ZigbeePoint& point = points_[i];
		// A function without a periodic report (max 0: on change only, 65535: no reports) promises nothing;
		// neither does one on an endpoint the device has not described (its reporting is not configured).
		if (point.ieee == ieee && configures_reporting(point.function) && point.reporting.periodic() &&
			device->endpoint(point.endpoint) != nullptr) {
			uint32_t max_s = point.reporting.max_s;
			if (shortest == 0 || max_s < shortest) {
				shortest = max_s;
			}
		}
	}
	return shortest * kStaleFactor;
}

long ZigbeeAdapter::find_point(Ieee ieee, uint8_t endpoint, Function function) const {
	for (size_t i = 0; i < point_count_; ++i) {
		const ZigbeePoint& p = points_[i];
		if (p.ieee == ieee && p.endpoint == endpoint && p.function == function) {
			return static_cast<long>(i);
		}
	}
	return -1;
}

void ZigbeeAdapter::traffic(ZigbeeTraffic::Kind kind, const ZigbeeDevice& device, uint8_t endpoint, uint16_t cluster,
	uint16_t code, uint16_t datapoint, const Value* value) {
	if (traffic_ == nullptr) {
		return;
	}
	ZigbeeTraffic t;
	t.kind = kind;
	t.ieee = device.ieee;
	t.short_addr = device.short_addr;
	t.endpoint = endpoint;
	t.cluster = cluster;
	t.code = code;
	t.datapoint = datapoint;
	t.value = value;
	traffic_->zigbee_traffic(t);
}

uint16_t ZigbeeAdapter::report_point(const ZigbeeDevice& device, uint8_t endpoint, Function function, const Value& value) {
	if (!loaded_) {
		return kNoId;
	}
	long index = find_point(device.ieee, endpoint, function);
	if (index < 0) {
		char ieee[kIeeeText];
		format_ieee(device.ieee, ieee);
		Log::debug(kTag, "%s endpoint %u: %s not mapped to a datapoint", ieee, static_cast<unsigned>(endpoint),
			function_info(function).key);
		return kNoId;
	}
	if (ids_[index] != kNoId) {
		model_.report(*this, ids_[index], value);
	}
	return ids_[index];
}

void ZigbeeAdapter::check_stale() {
	read_child_table();
	uint64_t now = clock_.monotonic_ms();
	bool seen_due = false;
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		seen_due = seen_due || device.last_seen_s - device.saved_seen_s >= kSeenSaveS;
	}
	if (seen_due && persistent_ && !save_database()) {
		// Logged; tried again in kSeenSaveS, not at every check.
		for (size_t i = 0; i < database_.size(); ++i) {
			database_.at(i).saved_seen_s = database_.at(i).last_seen_s;
		}
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		ZigbeeDevice& device = database_.at(i);
		uint32_t timeout_s = stale_timeout_s(device.ieee);
		if (device.stale || timeout_s == 0) {
			continue;
		}
		uint64_t since = device.seen_ms != 0 ? device.seen_ms : start_ms_;
		if (now - since < static_cast<uint64_t>(timeout_s) * 1000) {
			continue;
		}
		device.stale = true;
		char text[64];
		Log::warn(kTag, "%s silent for %u s (3 reporting intervals): its datapoints are stale",
			device_text(device, text, sizeof(text)), static_cast<unsigned>((now - since) / 1000));
		request_topology(); // where is it now?
		if (loaded_) {
			for (size_t p = 0; p < point_count_; ++p) {
				if (points_[p].ieee == device.ieee && ids_[p] != kNoId) {
					model_.report_stale(*this, ids_[p]);
				}
			}
		}
		notify(ZigbeeEvent::Kind::changed, device.ieee);
	}
}

void ZigbeeAdapter::schedule_refresh() {
	if (refresh_timer_.valid()) {
		return;
	}
	refresh_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kRefreshSpacingMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::refresh_due>(this, "zigbee.refresh_due"));
}

void ZigbeeAdapter::refresh_due() {
	refresh_timer_ = TimerHandle();
	ZigbeeDevice* device = nullptr;
	for (size_t i = 0; i < database_.size() && device == nullptr; ++i) {
		if (database_.at(i).refresh) {
			device = &database_.at(i);
		}
	}
	if (device == nullptr) {
		return;
	}
	device->refresh = false;
	if (device->short_addr != kNoShortAddress) {
		// One Read Attributes per endpoint and cluster, started at the group's first point.
		// Only the endpoints the device described (a binding may name one an incomplete interview
		// has not learnt yet; the retry reads it).
		auto readable = [&](const ZigbeePoint& p) {
			return p.ieee == device->ieee && (p.flags() & DatapointFlags::kReadOnStart) != 0 &&
				device->endpoint(p.endpoint) != nullptr;
		};
		auto same_group = [](const ZigbeePoint& a, const ZigbeePoint& b) {
			return a.endpoint == b.endpoint && a.cluster() == b.cluster();
		};
		for (size_t i = 0; i < point_count_; ++i) {
			const ZigbeePoint& first = points_[i];
			if (!readable(first)) {
				continue;
			}
			bool earlier = false;
			for (size_t j = 0; j < i && !earlier; ++j) {
				earlier = readable(points_[j]) && same_group(points_[j], first);
			}
			if (earlier) {
				continue;
			}
			uint16_t attributes[kMaxReadAttributes];
			size_t count = 0;
			for (size_t j = i; j < point_count_ && count < kMaxReadAttributes; ++j) {
				if (readable(points_[j]) && same_group(points_[j], first)) {
					attributes[count++] = points_[j].attribute();
				}
			}
			driver_.read_attributes(device->ieee, device->short_addr, first.endpoint, first.cluster(), attributes, count);
		}
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		if (database_.at(i).refresh) {
			schedule_refresh();
			break;
		}
	}
}

// ---- The interview --------------------------------------------------------------------------

void ZigbeeAdapter::start_pending_interview() {
	if (interview_.active) {
		return;
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		ZigbeeDevice& device = database_.at(i);
		if (device.interview_pending) {
			device.interview_pending = false;
			if (device.short_addr == kNoShortAddress) {
				continue;
			}
			start_interview(device);
			return;
		}
	}
}

void ZigbeeAdapter::start_interview(ZigbeeDevice& device) {
	bool retry = device.interview_retry && device.incomplete();
	device.interview_retry = false;
	device.interview_ms = clock_.monotonic_ms();
	loop_.cancel(interview_.timer);
	interview_.active = true;
	interview_.ieee = device.ieee;
	interview_.step = Step::active_endpoints;
	interview_.attempts = 0;
	interview_.index = 0;
	interview_.queried_count = 0;
	interview_.names_wanted = true;
	interview_.first_new = 0;
	interview_.result = ZigbeeDevice();
	interview_.result.ieee = device.ieee;
	interview_.timer = TimerHandle();
	char text[64];
	device_text(device, text, sizeof(text));
	if (!retry) {
		Log::info(kTag, "interviewing %s (short 0x%04x)", text, static_cast<unsigned>(device.short_addr));
		notify(ZigbeeEvent::Kind::interviewing, device.ieee);
		send_step();
		return;
	}
	// The retry of an incomplete interview starts from its partial result and asks for the rest.
	ZigbeeDevice& result = interview_.result;
	std::memcpy(result.manufacturer, device.manufacturer, sizeof(result.manufacturer));
	std::memcpy(result.model, device.model, sizeof(result.model));
	result.endpoint_count = device.endpoint_count;
	for (size_t i = 0; i < device.endpoint_count; ++i) {
		result.endpoints[i] = device.endpoints[i];
	}
	interview_.first_new = device.endpoint_count;
	interview_.names_wanted = device.missing_names;
	for (size_t i = 0; i < device.missing_count; ++i) {
		interview_.queried[i] = device.missing[i];
	}
	interview_.queried_count = device.missing_count;
	char why[96];
	Log::info(kTag, "%s heard again (short 0x%04x): retrying its incomplete interview (%s)", text,
		static_cast<unsigned>(device.short_addr), missing_text(device, why, sizeof(why)));
	notify(ZigbeeEvent::Kind::interviewing, device.ieee);
	if (interview_.queried_count > 0) {
		next_simple_descriptor();
	} else {
		start_basic();
	}
}

void ZigbeeAdapter::send_step() {
	loop_.cancel(interview_.timer);
	ZigbeeDevice* device = database_.find(interview_.ieee);
	if (device == nullptr) {
		end_interview();
		return;
	}
	Ieee ieee = device->ieee;
	uint16_t short_addr = device->short_addr;
	bool sent = false;
	switch (interview_.step) {
		case Step::active_endpoints:
			sent = driver_.request_active_endpoints(ieee, short_addr);
			break;
		case Step::simple_descriptor:
			sent = driver_.request_simple_descriptor(ieee, short_addr, interview_.queried[interview_.index]);
			break;
		case Step::basic: {
			static const uint16_t kNames[] = {attribute::kManufacturerName, attribute::kModelIdentifier};
			sent = driver_.read_attributes(ieee, short_addr, interview_.result.endpoints[interview_.index].id,
				cluster::kBasic, kNames, 2);
			break;
		}
		case Step::zone: {
			static const uint16_t kZone[] = {attribute::kZoneType};
			sent = driver_.read_attributes(ieee, short_addr, interview_.result.endpoints[interview_.index].id,
				cluster::kIasZone, kZone, 1);
			break;
		}
	}
	if (!sent) {
		Log::warn(kTag, "interview request (%s) refused by the stack; repeated after the timeout",
			step_name(static_cast<uint8_t>(interview_.step)));
	}
	interview_.timer = loop_.schedule_at(clock_.monotonic_ms() + kInterviewTimeoutMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::step_timeout>(this, "zigbee.step_timeout"));
}

void ZigbeeAdapter::step_timeout() {
	interview_.timer = TimerHandle();
	if (!interview_.active) {
		return;
	}
	++interview_.attempts;
	if (interview_.attempts < kInterviewAttempts) {
		send_step();
		return;
	}
	switch (interview_.step) {
		case Step::active_endpoints:
			fail_interview("no answer to the active endpoint request");
			break;
		case Step::simple_descriptor: {
			// A sleepy device may have fallen asleep: the interview goes on and ends incomplete.
			ZigbeeDevice& result = interview_.result;
			uint8_t id = interview_.queried[interview_.index];
			Log::warn(kTag, "no simple descriptor for endpoint %u: the interview stays incomplete",
				static_cast<unsigned>(id));
			if (result.missing_count < ZigbeeDevice::kMaxMissing) {
				result.missing[result.missing_count++] = id;
			}
			++interview_.index;
			next_simple_descriptor();
			break;
		}
		case Step::basic:
			Log::warn(kTag, "no manufacturer and model from the Basic cluster: the interview stays incomplete");
			interview_.result.missing_names = true;
			start_zone(interview_.first_new);
			break;
		case Step::zone:
			Log::warn(kTag, "no IAS zone type for endpoint %u: its zone is named \"alarm\"",
				static_cast<unsigned>(interview_.result.endpoints[interview_.index].id));
			start_zone(static_cast<uint8_t>(interview_.index + 1));
			break;
	}
}

void ZigbeeAdapter::next_simple_descriptor() {
	interview_.attempts = 0;
	if (interview_.index < interview_.queried_count) {
		interview_.step = Step::simple_descriptor;
		send_step();
		return;
	}
	if (interview_.result.endpoint_count == 0) {
		fail_interview(interview_.result.missing_count > 0 ? "no simple descriptor answered"
			: "no endpoint of the Home Automation or Light Link profile");
		return;
	}
	if (interview_.names_wanted) {
		start_basic();
	} else {
		start_zone(interview_.first_new);
	}
}

void ZigbeeAdapter::start_basic() {
	interview_.step = Step::basic;
	interview_.attempts = 0;
	interview_.index = 0;
	for (uint8_t i = 0; i < interview_.result.endpoint_count; ++i) {
		if (interview_.result.endpoints[i].has_in(cluster::kBasic)) {
			interview_.index = i;
			break;
		}
	}
	send_step();
}

void ZigbeeAdapter::start_zone(uint8_t from_index) {
	for (uint8_t i = from_index; i < interview_.result.endpoint_count; ++i) {
		if (interview_.result.endpoints[i].has_in(cluster::kIasZone)) {
			interview_.step = Step::zone;
			interview_.attempts = 0;
			interview_.index = i;
			send_step();
			return;
		}
	}
	finish_interview();
}

void ZigbeeAdapter::end_interview() {
	loop_.cancel(interview_.timer);
	interview_.timer = TimerHandle();
	interview_.active = false;
}

void ZigbeeAdapter::fail_interview(const char* reason) {
	char ieee[kIeeeText];
	format_ieee(interview_.ieee, ieee);
	Log::warn(kTag, "interview of %s failed at step %s: %s; it is tried again when the device is heard after %u s",
		ieee, step_name(static_cast<uint8_t>(interview_.step)), reason,
		static_cast<unsigned>(kInterviewRetryMs / 1000));
	end_interview();
	notify(ZigbeeEvent::Kind::interview_failed, interview_.ieee);
	start_pending_interview();
}

void ZigbeeAdapter::finish_interview() {
	end_interview();
	ZigbeeDevice* device = database_.find(interview_.ieee);
	if (device == nullptr) {
		start_pending_interview();
		return;
	}
	ZigbeeDevice& result = interview_.result;
	bool incomplete = result.missing_count > 0 || result.missing_names;
	// What the device knew before and this interview could not confirm stays: an endpoint that did
	// not answer keeps its earlier descriptor, unanswered names keep the earlier names.
	for (size_t m = 0; m < result.missing_count && result.endpoint_count < ZigbeeDevice::kMaxEndpoints; ++m) {
		const ZigbeeEndpoint* earlier = device->endpoint(result.missing[m]);
		if (earlier != nullptr && result.endpoint(result.missing[m]) == nullptr) {
			result.endpoints[result.endpoint_count++] = *earlier;
		}
	}
	if (!result.missing_names || result.manufacturer[0] != '\0' || result.model[0] != '\0') {
		std::memcpy(device->manufacturer, result.manufacturer, sizeof(device->manufacturer));
		std::memcpy(device->model, result.model, sizeof(device->model));
	}
	device->endpoint_count = result.endpoint_count;
	for (size_t i = 0; i < result.endpoint_count; ++i) {
		device->endpoints[i] = result.endpoints[i];
	}
	device->interviewed = !incomplete;
	device->missing_count = result.missing_count;
	for (size_t i = 0; i < result.missing_count; ++i) {
		device->missing[i] = result.missing[i];
	}
	device->missing_names = result.missing_names;
	if (incomplete) {
		// The retry waits for the device's next frame after kInterviewRetryMs, counted from here.
		device->interview_ms = clock_.monotonic_ms();
	}
	save_database();
	if (started_) {
		rebuild(nullptr);
	}
	ZigbeePoint mapped[kMaxDevicePoints];
	size_t points = device_points(*device, mapped, kMaxDevicePoints);
	char ieee[kIeeeText];
	format_ieee(device->ieee, ieee);
	if (incomplete) {
		char why[96];
		Log::warn(kTag, "interview of %s incomplete (%s): \"%s\" \"%s\", %u endpoint(s), %u datapoint(s) in use; the rest "
			"is asked for when the device is heard after %u s", ieee, missing_text(*device, why, sizeof(why)),
			device->manufacturer, device->model, static_cast<unsigned>(device->endpoint_count),
			static_cast<unsigned>(points), static_cast<unsigned>(kInterviewRetryMs / 1000));
	} else {
		Log::info(kTag, "device %s interviewed: \"%s\" \"%s\", %u endpoint(s), %u datapoint(s)", ieee,
			device->manufacturer, device->model, static_cast<unsigned>(device->endpoint_count),
			static_cast<unsigned>(points));
	}
	setup_reporting(*device);
	device->refresh = true;
	schedule_refresh();
	notify(incomplete ? ZigbeeEvent::Kind::interview_incomplete : ZigbeeEvent::Kind::interviewed, device->ieee);
	start_pending_interview();
}

const char* ZigbeeAdapter::missing_text(const ZigbeeDevice& device, char* buffer, size_t size) {
	if (size == 0) {
		return buffer;
	}
	buffer[0] = '\0';
	size_t used = 0;
	auto append = [&](const char* text) {
		int n = std::snprintf(buffer + used, size - used, "%s", text);
		if (n > 0) {
			used += static_cast<size_t>(n) < size - used ? static_cast<size_t>(n) : size - used - 1;
		}
	};
	if (device.missing_count > 0) {
		append(device.missing_count == 1 ? "endpoint " : "endpoints ");
		for (size_t i = 0; i < device.missing_count; ++i) {
			char id[8];
			std::snprintf(id, sizeof(id), "%s%u", i > 0 ? ", " : "", static_cast<unsigned>(device.missing[i]));
			append(id);
		}
		append(" did not answer");
	}
	if (device.missing_names) {
		append(device.missing_count > 0 ? "; " : "");
		append("manufacturer and model did not answer");
	}
	return buffer;
}

void ZigbeeAdapter::setup_reporting(const ZigbeeDevice& device) {
	struct Bound {
		uint8_t endpoint;
		uint16_t cluster;
	};
	Bound bound[kMaxBindings];
	size_t bound_count = 0;
	size_t configured = 0;
	auto bind_once = [&](uint8_t endpoint, uint16_t cluster) {
		for (size_t i = 0; i < bound_count; ++i) {
			if (bound[i].endpoint == endpoint && bound[i].cluster == cluster) {
				return;
			}
		}
		if (bound_count >= kMaxBindings) {
			return;
		}
		bound[bound_count++] = Bound{endpoint, cluster};
		driver_.bind(device.ieee, device.short_addr, endpoint, cluster);
	};
	// The device's mapping, not the loaded points: with the image service the model follows later.
	ZigbeePoint points[kMaxDevicePoints];
	size_t count = device_points(device, points, kMaxDevicePoints);
	for (size_t i = 0; i < count; ++i) {
		const ZigbeePoint& point = points[i];
		const FunctionInfo& info = function_info(point.function);
		if (point.function == Function::action) {
			const ZigbeeEndpoint* endpoint = device.endpoint(point.endpoint);
			if (endpoint == nullptr || endpoint->has_out(cluster::kOnOff)) {
				bind_once(point.endpoint, cluster::kOnOff);
			}
			if (endpoint != nullptr && endpoint->has_out(cluster::kLevelControl)) {
				bind_once(point.endpoint, cluster::kLevelControl);
			}
			continue;
		}
		if (point.function == Function::ias_zone) {
			continue; // Zone notifications go to the enrolled CIE address, no binding needed.
		}
		bind_once(point.endpoint, info.cluster);
		if (configures_reporting(point.function)) {
			ReportingConfig config{info.attribute, info.zcl_type, point.reporting.min_s, point.reporting.max_s,
				point.reporting.change};
			driver_.configure_reporting(device.ieee, device.short_addr, point.endpoint, info.cluster, config);
			++configured;
		}
	}
	char text[64];
	Log::info(kTag, "%s: %u binding(s) to the coordinator and the reporting of %u attribute(s) requested "
		"(refusals and timeouts are logged as they come back)",
		device_text(device, text, sizeof(text)), static_cast<unsigned>(bound_count), static_cast<unsigned>(configured));
}

void ZigbeeAdapter::reconfigure_reporting(const ZigbeePoint* old_table, size_t old_count) {
	for (size_t d = 0; d < database_.size(); ++d) {
		const ZigbeeDevice& device = database_.at(d);
		// A device not interviewed (or interviewed now) is configured when its interview finishes; one
		// that left when it rejoins and is interviewed again.
		if (!device.has_result() || device.left || device.short_addr == kNoShortAddress ||
			(interview_.active && interview_.ieee == device.ieee)) {
			continue;
		}
		struct Bound {
			uint8_t endpoint;
			uint16_t cluster;
		};
		Bound bound[kMaxBindings];
		size_t bound_count = 0;
		size_t configured = 0;
		size_t refused = 0;
		auto check = [&](uint8_t endpoint, Function function, const Reporting& now) {
			if (!configures_reporting(function)) {
				return;
			}
			Reporting before;
			// A function the previous bindings did not make a point was never configured: configure it.
			if (reporting_in(old_table, old_count, device, endpoint, function, before) && before == now) {
				return;
			}
			const FunctionInfo& info = function_info(function);
			bool seen = false;
			for (size_t i = 0; i < bound_count && !seen; ++i) {
				seen = bound[i].endpoint == endpoint && bound[i].cluster == info.cluster;
			}
			if (!seen && bound_count < kMaxBindings) {
				bound[bound_count++] = Bound{endpoint, info.cluster};
				driver_.bind(device.ieee, device.short_addr, endpoint, info.cluster);
			}
			ReportingConfig config{info.attribute, info.zcl_type, now.min_s, now.max_s, now.change};
			if (driver_.configure_reporting(device.ieee, device.short_addr, endpoint, info.cluster, config)) {
				++configured;
			} else {
				++refused;
			}
		};
		if (device_has_overrides(device.ieee)) {
			for (size_t i = 0; i < override_count_; ++i) {
				const ZigbeePoint& point = overrides_[i];
				if (point.ieee == device.ieee && device.endpoint(point.endpoint) != nullptr) {
					check(point.endpoint, point.function, point.reporting);
				}
			}
		} else {
			for (size_t e = 0; e < device.endpoint_count; ++e) {
				const ZigbeeEndpoint& endpoint = device.endpoints[e];
				for (size_t f = 0; f < kFunctionCount; ++f) {
					Function function = static_cast<Function>(f);
					const FunctionInfo& info = function_info(function);
					if (info.server && endpoint.has_in(info.cluster)) {
						check(endpoint.id, function, default_reporting(function));
					}
				}
			}
		}
		if (configured + refused == 0) {
			continue;
		}
		char text[64];
		device_text(device, text, sizeof(text));
		if (refused > 0) {
			Log::warn(kTag, "%s: the new bindings change the reporting of %u attribute(s), the stack refused %u of the "
				"requests: those keep their previous configuration", text, static_cast<unsigned>(configured + refused),
				static_cast<unsigned>(refused));
		} else {
			Log::info(kTag, "%s: the new bindings change the reporting of %u attribute(s): configured again "
				"(refusals and timeouts are logged as they come back)", text, static_cast<unsigned>(configured));
		}
	}
}

// ---- Platform events ------------------------------------------------------------------------

void ZigbeeAdapter::network_state(bool up, uint16_t pan_id, uint8_t channel, Ieee extended_pan) {
	network_.up = up;
	network_.pan_id = pan_id;
	network_.channel = channel;
	network_.extended_pan = extended_pan;
	if (up) {
		char ext[kIeeeText];
		format_ieee(extended_pan, ext);
		Log::info(kTag, "network up: PAN id 0x%04x, channel %u, extended PAN id %s", static_cast<unsigned>(pan_id),
			static_cast<unsigned>(channel), ext);
		trust_known_devices();
		size_t held = commands_held();
		if (held > 0 && !held_timer_.valid()) {
			Log::info(kTag, "sending %u held command(s)", static_cast<unsigned>(held));
			held_next_ = 0;
			send_held();
		}
	} else {
		loop_.cancel(held_timer_);
		held_timer_ = TimerHandle();
		network_.permit_until_ms = 0;
		Log::warn(kTag, "network down");
	}
	notify(ZigbeeEvent::Kind::network, 0);
}

void ZigbeeAdapter::permit_join_status(uint8_t seconds) {
	if (seconds > 0) {
		network_.permit_until_ms = clock_.monotonic_ms() + static_cast<uint64_t>(seconds) * 1000;
		Log::info(kTag, "network open for joining for %u s: put the device into pairing mode",
			static_cast<unsigned>(seconds));
	} else {
		network_.permit_until_ms = 0;
		Log::info(kTag, "network closed for joining");
	}
	notify(ZigbeeEvent::Kind::permit, 0, seconds);
}

void ZigbeeAdapter::device_announced(NodeAddress address) {
	if (address.ieee == 0) {
		Log::warn(kTag, "device announce without IEEE address (short 0x%04x) ignored",
			static_cast<unsigned>(address.short_addr));
		return;
	}
	const ZigbeeDevice* before = database_.find(address.ieee);
	bool known = before != nullptr;
	// The trust center's notice of this device just before (it moved the short address already).
	bool notice = known && notice_ieee_ == address.ieee && clock_.monotonic_ms() - notice_ms_ <= kNoticeBeforeAnnounceMs;
	uint16_t old_short = notice ? notice_old_short_ : known ? before->short_addr : kNoShortAddress;
	if (notice) {
		notice_ieee_ = 0; // one announce belongs to a notice
	}
	ZigbeeDevice* device = resolve(address, true);
	if (device == nullptr) {
		return;
	}
	char text[64];
	if (known && old_short != kNoShortAddress && old_short != address.short_addr) {
		// A rejoin with a new address (homecontrol #98): a NWK rejoin through a router that sends the
		// trust center no Update Device leaves the announce as its only trace.
		char parent[64] = "";
		if (notice && device->parent_short != kNoShortAddress) {
			char named[64];
			std::snprintf(parent, sizeof(parent), " through %s", parent_text(device->parent_short, named, sizeof(named)));
		}
		Log::info(kTag, "%s rejoined with a new address: short 0x%04x -> 0x%04x%s, %s%s%s", device_text(*device, text,
			sizeof(text)), static_cast<unsigned>(old_short), static_cast<unsigned>(address.short_addr), parent,
			notice ? "after the trust center's " : "no trust-center notice (its parent is looked up in the routers' tables)",
			notice ? update_status_name(notice_status_) : "", notice ? " notice" : "");
		if (!notice) {
			device->parent_short = kNoShortAddress; // the old parent is history
			device->set(ZigbeeDevice::kParentListed, false);
		}
	} else if (known) {
		Log::info(kTag, "%s announced itself (short 0x%04x)", device_text(*device, text, sizeof(text)),
			static_cast<unsigned>(address.short_addr));
	}
	request_topology();
	if (rejoin_.ieee == device->ieee) {
		rejoin_returned(*device);
	}
	if (device->left && device->has_result()) {
		// It left (a factory reset) and joined again: its endpoints may have changed.
		device->interview_pending = true;
		device->interview_retry = false;
	}
	bool was_left = device->left;
	device->left = false;
	touch(*device);
	if (known && was_left) {
		notify(ZigbeeEvent::Kind::changed, device->ieee);
	}
	start_pending_interview();
}

void ZigbeeAdapter::device_announced(NodeAddress address, uint8_t capability) {
	device_announced(address);
	ZigbeeDevice* device = address.ieee != 0 ? database_.find(address.ieee) : nullptr;
	if (device != nullptr) {
		device->set(ZigbeeDevice::kRouter, (capability & kCapabilityRouter) != 0);
	}
}

void ZigbeeAdapter::device_updated(NodeAddress address, uint8_t status, uint8_t tc_action, uint16_t parent_short) {
	if (address.ieee == 0) {
		Log::warn(kTag, "%s without IEEE address (short 0x%04x) ignored", update_status_name(status),
			static_cast<unsigned>(address.short_addr));
		return;
	}
	const ZigbeeDevice* before = database_.find(address.ieee);
	notice_old_short_ = before != nullptr ? before->short_addr : kNoShortAddress;
	ZigbeeDevice* device = resolve(address, false);
	char text[64];
	char parent[64];
	node_text(address, text, sizeof(text));
	parent_text(parent_short, parent, sizeof(parent));
	// A refusal is a device the trust center did not take back (homecontrol #99): warn.
	Log::write(tc_action == kTcAuthorize ? LogLevel::info : LogLevel::warn, kTag,
		"%s: %s (short 0x%04x) through %s, trust center: %s", text, update_status_name(status),
		static_cast<unsigned>(address.short_addr), parent, tc_action_name(tc_action));
	if (device != nullptr) {
		device->parent_short = status == kUpdateLeft ? kNoShortAddress : parent_short;
		device->set(ZigbeeDevice::kParentListed, false);
		if (status == kUpdateLeft) {
			trust(*device, "after its leave"); // the trust center forgot it; a known device may rejoin
		}
	}
	notice_ieee_ = address.ieee;
	notice_status_ = status;
	notice_ms_ = clock_.monotonic_ms();
	if (status == kUpdateUnsecuredJoin) {
		device_joined(address);
	}
}

void ZigbeeAdapter::device_authorized(NodeAddress address, uint8_t type, uint8_t status) {
	char text[64];
	node_text(address, text, sizeof(text));
	Log::write(status == 0 ? LogLevel::info : LogLevel::warn, kTag, "%s (short 0x%04x): authorization (%s): %s", text,
		static_cast<unsigned>(address.short_addr), authorization_name(type), authorization_status_name(type, status));
	// The stack deletes a legacy device's provisional link key when it authorizes it (homecontrol #99).
	const ZigbeeDevice* device = address.ieee != 0 ? database_.find(address.ieee) : nullptr;
	if (device != nullptr && type == kAuthorizationLegacy && status == 0) {
		trust(*device, "after its authorization");
	}
}

void ZigbeeAdapter::device_associated(Ieee ieee) {
	NodeAddress address;
	address.ieee = ieee;
	char text[64];
	node_text(address, text, sizeof(text));
	Log::info(kTag, "%s associated with the coordinator", text);
}

void ZigbeeAdapter::radio(NodeAddress address, uint8_t lqi, int8_t rssi) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return;
	}
	device->set(ZigbeeDevice::kRadioKnown, true);
	device->lqi = lqi;
	device->rssi = rssi;
}

// ---- The coordinator's child table ----------------------------------------------------------

/// Collects one read of the neighbour table into the devices (on the loop's thread, while the
/// platform holds the stack: it only copies).
class ZigbeeAdapter::ChildTableReader : public ZigbeeNeighborVisitor {
public:
	explicit ChildTableReader(DeviceDatabase& database) : database_(database) {}

	void neighbor(const ZigbeeNeighbor& neighbor) override {
		for (size_t i = 0; i < database_.size(); ++i) {
			ZigbeeDevice& device = database_.at(i);
			if (neighbor.ieee != 0 ? device.ieee != neighbor.ieee : device.short_addr != neighbor.short_addr) {
				continue;
			}
			if (neighbor.router) {
				device.set(ZigbeeDevice::kRouter, true);
			}
			if (neighbor.relationship == ZigbeeNeighbor::Relationship::child) {
				child[i] = true;
				device.child_timeout_s = neighbor.timeout_s;
				device.child_expires_ms = neighbor.expires_ms;
			}
			return;
		}
	}

	/// Which devices (by index) the table lists as children.
	bool child[DeviceDatabase::kMaxDevices] = {};

private:
	DeviceDatabase& database_;
};

void ZigbeeAdapter::read_child_table() {
	if (!network_.up) {
		return;
	}
	// The remaining time of an entry that disappears, as the read before found it.
	uint32_t before_ms[DeviceDatabase::kMaxDevices];
	for (size_t i = 0; i < database_.size(); ++i) {
		before_ms[i] = database_.at(i).child_expires_ms;
	}
	ChildTableReader reader(database_);
	if (!driver_.read_neighbors(reader)) {
		Log::debug(kTag, "neighbour table not readable now: the children are read again in %u s",
			static_cast<unsigned>(kCheckPeriodMs / 1000));
		return;
	}
	uint64_t now = clock_.monotonic_ms();
	for (size_t i = 0; i < database_.size(); ++i) {
		ZigbeeDevice& device = database_.at(i);
		bool child = reader.child[i];
		if (child == device.has(ZigbeeDevice::kChild) && children_read_) {
			continue;
		}
		char text[64];
		device_text(device, text, sizeof(text));
		if (child) {
			Log::info(kTag, "%s %s a child of the coordinator (short 0x%04x, end-device timeout %u s, %u s left)", text,
				children_read_ ? "became" : "is", static_cast<unsigned>(device.short_addr),
				static_cast<unsigned>(device.child_timeout_s), static_cast<unsigned>(device.child_expires_ms / 1000));
		} else if (device.has(ZigbeeDevice::kChild)) {
			char seen[48] = "not heard since the start";
			if (device.seen_ms != 0) {
				std::snprintf(seen, sizeof(seen), "last heard %u s ago",
					static_cast<unsigned>((now - device.seen_ms) / 1000));
			}
			Log::info(kTag, "%s is no longer a child of the coordinator: removed from its child table (end-device timeout "
				"%u s, %u s were left %u s before; %s)", text, static_cast<unsigned>(device.child_timeout_s),
				static_cast<unsigned>(before_ms[i] / 1000), static_cast<unsigned>(kCheckPeriodMs / 1000), seen);
		}
		if (child != device.has(ZigbeeDevice::kChild)) {
			device.set(ZigbeeDevice::kChild, child);
			notify(ZigbeeEvent::Kind::changed, device.ieee);
		}
	}
	children_read_ = true;
}

// ---- The routers' neighbour tables ----------------------------------------------------------

void ZigbeeAdapter::request_topology() {
	if (!network_.up || !started_) {
		return;
	}
	if (topology_.active) {
		topology_.again = true;
		return;
	}
	if (topology_.timer.valid()) {
		return; // a pass is about to start
	}
	topology_.timer = loop_.schedule_at(clock_.monotonic_ms() + kTopologyDelayMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::topology_start>(this, "zigbee.topology_start"));
}

void ZigbeeAdapter::topology_tick() {
	request_topology();
}

void ZigbeeAdapter::topology_start() {
	topology_.timer = TimerHandle();
	if (!network_.up) {
		return;
	}
	topology_.active = true;
	topology_.again = false;
	topology_.listed = 0;
	topology_.answered = 0;
	topology_next_router(0);
}

void ZigbeeAdapter::topology_next_router(size_t from) {
	for (size_t i = from; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
		if (device.has(ZigbeeDevice::kRouter) && !device.left && device.short_addr != kNoShortAddress &&
			device.short_addr != 0x0000) {
			topology_.router = i;
			topology_.router_ieee = device.ieee;
			topology_.router_short = device.short_addr;
			topology_.start = 0;
			topology_.pages = 0;
			topology_send();
			return;
		}
	}
	topology_finish();
}

void ZigbeeAdapter::topology_send() {
	if (!driver_.request_neighbor_table(topology_.router_ieee, topology_.router_short, topology_.start)) {
		char text[64];
		Log::debug(kTag, "neighbour table request to %s refused by the stack",
			device_text(database_.at(topology_.router), text, sizeof(text)));
		topology_next_router(topology_.router + 1);
		return;
	}
	topology_.timer = loop_.schedule_at(clock_.monotonic_ms() + kTopologyTimeoutMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::topology_timeout>(this, "zigbee.topology_timeout"));
}

void ZigbeeAdapter::topology_timeout() {
	topology_.timer = TimerHandle();
	ZigbeeDevice& router = database_.at(topology_.router);
	if (!router.has(ZigbeeDevice::kTopologySilent)) {
		router.set(ZigbeeDevice::kTopologySilent, true);
		char text[64];
		Log::info(kTag, "%s did not answer the request for its neighbour table (entries from %u) within %u s: its "
			"children keep the parent known", device_text(router, text, sizeof(text)),
			static_cast<unsigned>(topology_.start), static_cast<unsigned>(kTopologyTimeoutMs / 1000));
	}
	topology_next_router(topology_.router + 1);
}

void ZigbeeAdapter::neighbor_table(NodeAddress router, uint8_t status, uint8_t total, uint8_t start_index,
	const ZigbeeNeighbor* entries, size_t count) {
	bool ours = topology_.active && topology_.timer.valid() && start_index == topology_.start &&
		(router.ieee != 0 ? router.ieee == topology_.router_ieee : router.short_addr == topology_.router_short);
	if (!ours) {
		Log::debug(kTag, "neighbour table of 0x%04x (from %u) outside a request ignored",
			static_cast<unsigned>(router.short_addr), static_cast<unsigned>(start_index));
		return;
	}
	loop_.cancel(topology_.timer);
	topology_.timer = TimerHandle();
	ZigbeeDevice& owner = database_.at(topology_.router);
	char text[64];
	if (status != 0) {
		Log::debug(kTag, "%s answered the request for its neighbour table with status 0x%02x",
			device_text(owner, text, sizeof(text)), static_cast<unsigned>(status));
		topology_next_router(topology_.router + 1);
		return;
	}
	if (owner.has(ZigbeeDevice::kTopologySilent)) {
		owner.set(ZigbeeDevice::kTopologySilent, false);
		Log::info(kTag, "%s answers the requests for its neighbour table again", device_text(owner, text, sizeof(text)));
	}
	for (size_t e = 0; e < count; ++e) {
		const ZigbeeNeighbor& entry = entries[e];
		if (entry.coordinator || entry.short_addr == 0x0000) {
			owner.coordinator_lqi = entry.lqi; // the router's view of its link to the coordinator
			owner.set(ZigbeeDevice::kCoordinatorLink, true);
			continue;
		}
		if (entry.relationship != ZigbeeNeighbor::Relationship::child) {
			continue;
		}
		for (size_t i = 0; i < database_.size(); ++i) {
			ZigbeeDevice& child = database_.at(i);
			if (entry.ieee != 0 ? child.ieee != entry.ieee : child.short_addr != entry.short_addr) {
				continue;
			}
			if (child.ieee == owner.ieee) {
				break;
			}
			topology_.listed |= 1u << i;
			bool moved = child.parent_short != owner.short_addr;
			child.parent_short = owner.short_addr;
			child.parent_lqi = entry.lqi;
			child.depth = entry.depth;
			if (moved || !child.has(ZigbeeDevice::kParentListed)) {
				char name[64];
				Log::info(kTag, "%s is a child of %s (the router's link quality of it %u, depth %u)",
					device_text(child, text, sizeof(text)), device_text(owner, name, sizeof(name)),
					static_cast<unsigned>(entry.lqi), static_cast<unsigned>(entry.depth));
				notify(ZigbeeEvent::Kind::changed, child.ieee);
			}
			child.set(ZigbeeDevice::kParentListed, true);
			break;
		}
	}
	++topology_.pages;
	unsigned next = static_cast<unsigned>(start_index) + static_cast<unsigned>(count);
	if (count > 0 && next < total && next <= 0xFF && topology_.pages < kTopologyMaxPages) {
		topology_.start = static_cast<uint8_t>(next);
		topology_send();
		return;
	}
	topology_.answered |= 1u << topology_.router;
	topology_next_router(topology_.router + 1);
}

void ZigbeeAdapter::topology_finish() {
	// A device whose parent router answered completely without listing it has left that router.
	for (size_t i = 0; i < database_.size(); ++i) {
		ZigbeeDevice& device = database_.at(i);
		if (!device.has(ZigbeeDevice::kParentListed) || (topology_.listed & (1u << i)) != 0) {
			continue;
		}
		for (size_t p = 0; p < database_.size(); ++p) {
			if (database_.at(p).short_addr == device.parent_short && (topology_.answered & (1u << p)) != 0) {
				char text[64];
				char name[64];
				Log::info(kTag, "%s is no longer a child of %s", device_text(device, text, sizeof(text)),
					device_text(database_.at(p), name, sizeof(name)));
				device.parent_short = kNoShortAddress;
				device.set(ZigbeeDevice::kParentListed, false);
				notify(ZigbeeEvent::Kind::changed, device.ieee);
				break;
			}
		}
	}
	topology_.active = false;
	if (topology_.again) {
		topology_.again = false;
		request_topology();
	}
}

void ZigbeeAdapter::topology_abort() {
	if (!topology_.active) {
		return;
	}
	loop_.cancel(topology_.timer);
	topology_.timer = TimerHandle();
	topology_.active = false;
	topology_.again = false;
	request_topology(); // a fresh pass over the new indices
}

void ZigbeeAdapter::device_joined(NodeAddress address) {
	if (address.ieee == 0) {
		return;
	}
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return; // a new device: the announce that follows adds and interviews it
	}
	device->left = false;
	// A device with endpoints (interviewed or incomplete) and no full interview due yet; a due retry
	// of an incomplete interview becomes a full one.
	bool full_due = device->interview_pending && !device->interview_retry;
	if (device->has_result() && !full_due && !(interview_.active && interview_.ieee == device->ieee)) {
		// A fresh join of a known device means a factory reset: the device forgot the bindings and
		// the reporting the first interview configured, and a sleepy device listens only now, right
		// after the join. The interview starts with its announce (it holds the network key then); if
		// the announce came first, right away.
		char text[64];
		Log::info(kTag, "%s joined afresh (short 0x%04x): it was reset, interviewing it again",
			device_text(*device, text, sizeof(text)), static_cast<unsigned>(address.short_addr));
		device->interview_pending = true;
		device->interview_retry = false;
		notify(ZigbeeEvent::Kind::joined, device->ieee);
		if (device->seen_ms != 0 && clock_.monotonic_ms() - device->seen_ms < kAnnounceBeforeJoinMs) {
			start_pending_interview();
		}
	}
}

void ZigbeeAdapter::device_left_to_rejoin(NodeAddress address) {
	char text[64];
	node_text(address, text, sizeof(text));
	Log::info(kTag, "%s (short 0x%04x) left to rejoin", text, static_cast<unsigned>(address.short_addr));
	const ZigbeeDevice* device = resolve(address, false);
	if (device != nullptr) {
		trust(*device, "after its leave"); // the stack forgot the child's link key with its leave
	}
}

void ZigbeeAdapter::leave_answered(NodeAddress address, uint8_t status) {
	ZigbeeDevice* device = resolve(address, false);
	bool requested = rejoin_.ieee != 0 &&
		(device != nullptr ? device->ieee == rejoin_.ieee : address.short_addr == rejoin_.short_addr);
	char text[64];
	if (device != nullptr) {
		device_text(*device, text, sizeof(text));
	} else {
		node_text(address, text, sizeof(text));
	}
	if (!requested) {
		Log::info(kTag, "%s (short 0x%04x) answered the leave request: status 0x%02x", text,
			static_cast<unsigned>(address.short_addr), static_cast<unsigned>(status));
		return;
	}
	if (status == kLeaveSuccess) {
		rejoin_.accepted = true;
		Log::info(kTag, "%s took the request to leave and rejoin", text);
	} else if (rejoin_.sends < kRejoinSends) {
		Log::info(kTag, "%s did not take the request to leave and rejoin (status 0x%02x): asked again when it is next "
			"heard", text, static_cast<unsigned>(status));
	} else {
		Log::warn(kTag, "%s did not take the request to leave and rejoin (status 0x%02x), requested %u times", text,
			static_cast<unsigned>(status), static_cast<unsigned>(rejoin_.sends));
	}
	notify(ZigbeeEvent::Kind::changed, rejoin_.ieee);
}

void ZigbeeAdapter::device_left(NodeAddress address) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		Log::info(kTag, "an unknown device (short 0x%04x) left the network", static_cast<unsigned>(address.short_addr));
		return;
	}
	device->left = true;
	device->parent_short = kNoShortAddress;
	device->set(ZigbeeDevice::kParentListed, false);
	char text[64];
	Log::info(kTag, "%s left the network (it stays in the database; `zigbee remove` deletes it)",
		device_text(*device, text, sizeof(text)));
	// No longer known: no trust-center rejoin for it (homecontrol #99); it comes back by a new pairing.
	if (!driver_.forget_device(device->ieee)) {
		Log::warn(kTag, "%s: the trust center's link key for it could not be dropped: its trust-center rejoin is still "
			"accepted", text);
	}
	notify(ZigbeeEvent::Kind::left, device->ieee);
	if (interview_.active && interview_.ieee == device->ieee) {
		fail_interview("the device left");
	}
	if (!device->stale) {
		device->stale = true;
		if (loaded_) {
			for (size_t p = 0; p < point_count_; ++p) {
				if (points_[p].ieee == device->ieee && ids_[p] != kNoId) {
					model_.report_stale(*this, ids_[p]);
				}
			}
		}
	}
}

void ZigbeeAdapter::active_endpoints(NodeAddress address, uint8_t status, const uint8_t* endpoints, size_t count) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return;
	}
	touch(*device);
	if (!interview_.active || interview_.ieee != device->ieee || interview_.step != Step::active_endpoints) {
		Log::debug(kTag, "active endpoints outside an interview ignored");
		return;
	}
	if (status != 0) {
		Log::warn(kTag, "active endpoint request answered with status 0x%02x", static_cast<unsigned>(status));
		return; // the timeout repeats it
	}
	if (count == 0) {
		fail_interview("the device has no active endpoint");
		return;
	}
	if (count > kMaxQueriedEndpoints) {
		Log::warn(kTag, "the device has %u endpoints: the first %u are interviewed", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxQueriedEndpoints));
		count = kMaxQueriedEndpoints;
	}
	for (size_t i = 0; i < count; ++i) {
		interview_.queried[i] = endpoints[i];
	}
	interview_.queried_count = static_cast<uint8_t>(count);
	interview_.index = 0;
	next_simple_descriptor();
}

void ZigbeeAdapter::simple_descriptor(NodeAddress address, uint8_t status, const SimpleDescriptor& descriptor) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return;
	}
	touch(*device);
	if (!interview_.active || interview_.ieee != device->ieee || interview_.step != Step::simple_descriptor ||
		descriptor.endpoint != interview_.queried[interview_.index]) {
		Log::debug(kTag, "simple descriptor outside an interview ignored");
		return;
	}
	if (status != 0) {
		Log::warn(kTag, "simple descriptor of endpoint %u answered with status 0x%02x: skipped",
			static_cast<unsigned>(descriptor.endpoint), static_cast<unsigned>(status));
	} else if (descriptor.profile != kHomeAutomationProfile && descriptor.profile != kLightLinkProfile) {
		Log::info(kTag, "endpoint %u has profile 0x%04x: not a device function, ignored",
			static_cast<unsigned>(descriptor.endpoint), static_cast<unsigned>(descriptor.profile));
	} else if (interview_.result.endpoint_count >= ZigbeeDevice::kMaxEndpoints) {
		Log::warn(kTag, "endpoint %u dropped: %u endpoints are kept per device", static_cast<unsigned>(descriptor.endpoint),
			static_cast<unsigned>(ZigbeeDevice::kMaxEndpoints));
	} else {
		ZigbeeEndpoint& endpoint = interview_.result.endpoints[interview_.result.endpoint_count++];
		endpoint = ZigbeeEndpoint();
		endpoint.id = descriptor.endpoint;
		endpoint.profile = descriptor.profile;
		endpoint.device = descriptor.device;
		size_t dropped = endpoint.set_clusters(descriptor.in, descriptor.in_count, descriptor.out, descriptor.out_count);
		if (dropped > 0) {
			Log::warn(kTag, "endpoint %u: %u cluster(s) beyond the %u kept per direction dropped",
				static_cast<unsigned>(descriptor.endpoint), static_cast<unsigned>(dropped),
				static_cast<unsigned>(ZigbeeEndpoint::kMaxClusters));
		}
	}
	++interview_.index;
	next_simple_descriptor();
}

void ZigbeeAdapter::attributes(NodeAddress address, uint8_t endpoint, uint16_t cluster, const AttributeRecord* records,
	size_t count) {
	ZigbeeDevice* device = resolve(address, true);
	if (device == nullptr) {
		Log::debug(kTag, "attributes from unknown short 0x%04x ignored", static_cast<unsigned>(address.short_addr));
		return;
	}
	touch(*device);
	char ieee[kIeeeText];
	format_ieee(device->ieee, ieee);
	// The interview's reads.
	bool basic_step = interview_.active && interview_.ieee == device->ieee && interview_.step == Step::basic &&
		cluster == cluster::kBasic;
	bool zone_step = interview_.active && interview_.ieee == device->ieee && interview_.step == Step::zone &&
		cluster == cluster::kIasZone && endpoint == interview_.result.endpoints[interview_.index].id;
	bool zone_answered = false;
	for (size_t i = 0; i < count; ++i) {
		const AttributeRecord& record = records[i];
		if (record.status != zcl_status::kSuccess) {
			Log::debug(kTag, "%s endpoint %u cluster 0x%04x attribute 0x%04x: status 0x%02x", ieee,
				static_cast<unsigned>(endpoint), static_cast<unsigned>(cluster), static_cast<unsigned>(record.attribute),
				static_cast<unsigned>(record.status));
			zone_answered = zone_answered || (zone_step && record.attribute == attribute::kZoneType);
			continue;
		}
		ZclValue value;
		const char* why = nullptr;
		if (!decode_zcl(record.type, record.data, record.length, value, &why)) {
			Log::warn(kTag, "%s endpoint %u cluster 0x%04x attribute 0x%04x (type 0x%02x) not decoded: %s", ieee,
				static_cast<unsigned>(endpoint), static_cast<unsigned>(cluster), static_cast<unsigned>(record.attribute),
				static_cast<unsigned>(record.type), why);
			continue;
		}
		if (basic_step && value.is_string()) {
			if (record.attribute == attribute::kManufacturerName) {
				copy_text(interview_.result.manufacturer, sizeof(interview_.result.manufacturer), value.text);
			} else if (record.attribute == attribute::kModelIdentifier) {
				copy_text(interview_.result.model, sizeof(interview_.result.model), value.text);
			}
		}
		if (zone_step && record.attribute == attribute::kZoneType && !value.is_string()) {
			interview_.result.endpoints[interview_.index].zone_type = static_cast<uint16_t>(value.number);
			zone_answered = true;
		}
		const FunctionInfo* info = function_of(cluster, record.attribute);
		if (info == nullptr) {
			if (traffic_ != nullptr) {
				// Unmapped: the attribute's own value for the monitor.
				std::optional<Value> raw;
				if (value.is_string()) {
					raw = Value::of_string(value.text);
				} else if (value.number >= INT32_MIN && value.number <= INT32_MAX) {
					raw = Value::of_signed(static_cast<int32_t>(value.number));
				} else {
					raw = Value::of_real(static_cast<double>(value.number));
				}
				traffic(ZigbeeTraffic::Kind::report, *device, endpoint, cluster, record.attribute, ZigbeeTraffic::kNoDatapoint,
					raw ? &*raw : nullptr);
			}
			continue;
		}
		std::optional<Value> converted = attribute_value(info->function, value);
		if (!converted) {
			Log::debug(kTag, "%s endpoint %u: %s reported its invalid code", ieee, static_cast<unsigned>(endpoint),
				info->key);
			traffic(ZigbeeTraffic::Kind::report, *device, endpoint, cluster, record.attribute, ZigbeeTraffic::kNoDatapoint,
				nullptr);
			continue;
		}
		uint16_t id = report_point(*device, endpoint, info->function, *converted);
		traffic(ZigbeeTraffic::Kind::report, *device, endpoint, cluster, record.attribute, id, &*converted);
	}
	if (basic_step) {
		start_zone(interview_.first_new);
	} else if (zone_step && zone_answered) {
		driver_.ias_enroll(device->ieee, device->short_addr, endpoint);
		start_zone(static_cast<uint8_t>(interview_.index + 1));
	}
}

void ZigbeeAdapter::command_received(NodeAddress address, uint8_t endpoint, uint16_t cluster, uint8_t command,
	const uint8_t* payload, size_t length) {
	ZigbeeDevice* device = resolve(address, true);
	if (device == nullptr) {
		return;
	}
	touch(*device);
	if (cluster == cluster::kOnOff) {
		uint32_t action = 0;
		switch (command) {
			case 0x00: case 0x40: action = action::kOff; break;     // off, off with effect
			case 0x01: case 0x41: case 0x42: action = action::kOn; break; // on, on with recall, on with timed off
			case 0x02: action = action::kToggle; break;
			default:
				Log::debug(kTag, "On/Off command 0x%02x ignored", static_cast<unsigned>(command));
				traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, ZigbeeTraffic::kNoDatapoint, nullptr);
				return;
		}
		Value value = Value::of_enum(action);
		uint16_t id = report_point(*device, endpoint, Function::action, value);
		traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, id, &value);
		return;
	}
	if (cluster == cluster::kLevelControl) {
		uint32_t action = 0;
		switch (command) {
			case 0x01: case 0x05: // move (with on/off)
			case 0x02: case 0x06: // step (with on/off)
				if (length < 1 || payload == nullptr) {
					traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, ZigbeeTraffic::kNoDatapoint,
						nullptr);
					return;
				}
				action = payload[0] == 0 ? action::kDimUp : action::kDimDown;
				break;
			case 0x03: case 0x07: action = action::kDimStop; break;
			default:
				Log::debug(kTag, "Level Control command 0x%02x ignored", static_cast<unsigned>(command));
				traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, ZigbeeTraffic::kNoDatapoint, nullptr);
				return;
		}
		Value value = Value::of_enum(action);
		uint16_t id = report_point(*device, endpoint, Function::action, value);
		traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, id, &value);
		return;
	}
	if (cluster == cluster::kIasZone && command == 0x00) { // Zone Status Change Notification
		if (length < 2 || payload == nullptr) {
			traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, ZigbeeTraffic::kNoDatapoint, nullptr);
			return;
		}
		uint16_t status = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
		Value value = Value::of_bool((status & 0x1) != 0);
		uint16_t id = report_point(*device, endpoint, Function::ias_zone, value);
		traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, id, &value);
		return;
	}
	Log::debug(kTag, "command 0x%02x of cluster 0x%04x ignored", static_cast<unsigned>(command),
		static_cast<unsigned>(cluster));
	traffic(ZigbeeTraffic::Kind::action, *device, endpoint, cluster, command, ZigbeeTraffic::kNoDatapoint, nullptr);
}

void ZigbeeAdapter::reporting_configured(NodeAddress address, uint8_t endpoint, uint16_t cluster, uint8_t status) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return;
	}
	touch(*device);
	if (status != zcl_status::kSuccess) {
		char text[64];
		Log::warn(kTag, "%s refused the reporting configuration of cluster 0x%04x on endpoint %u (status 0x%02x): "
			"its values come when it reports on its own or is read", device_text(*device, text, sizeof(text)),
			static_cast<unsigned>(cluster), static_cast<unsigned>(endpoint), static_cast<unsigned>(status));
	}
}

void ZigbeeAdapter::bind_result(NodeAddress address, uint8_t status) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		return;
	}
	touch(*device);
	if (status != 0) {
		char text[64];
		Log::warn(kTag, "%s refused a binding to the coordinator (status 0x%02x)", device_text(*device, text, sizeof(text)),
			static_cast<unsigned>(status));
	}
}

// ---- Commands -------------------------------------------------------------------------------

bool ZigbeeAdapter::command(const Datapoint& datapoint, const Value& value) {
	long index = index_of(datapoint.id());
	if (index < 0) {
		Log::error(kTag, "command of '%s' refused: not a Zigbee datapoint", datapoint.name());
		return false;
	}
	const ZigbeePoint& point = points_[index];
	const ZigbeeDevice* device = database_.find(point.ieee);
	if (device == nullptr || device->short_addr == kNoShortAddress) {
		Log::warn(kTag, "command of '%s' refused: the device has not joined", datapoint.name());
		return false;
	}
	if (!point.commandable()) {
		Log::warn(kTag, "command of '%s' refused: read-only", datapoint.name());
		return false;
	}
	if (!command_valid(point, datapoint, value)) {
		return false;
	}
	if (!network_.up) {
		if (!hold_logged_) {
			hold_logged_ = true;
			Log::info(kTag, "holding commands until the Zigbee network is up (first: '%s')", datapoint.name());
		}
		char text[48];
		Log::debug(kTag, "holding the command %s for '%s'", value.format(text, sizeof(text)), datapoint.name());
		model_.set_pending(*this, datapoint.id(), true);
		return true;
	}
	if (!send_command(*device, point, datapoint, value)) {
		return false;
	}
	if (datapoint.pending()) {
		model_.set_pending(*this, datapoint.id(), false); // this value replaces the held one
	}
	return true;
}

bool ZigbeeAdapter::command_valid(const ZigbeePoint& point, const Datapoint& datapoint, const Value& value) const {
	switch (point.function) {
		case Function::on_off:
			return true;
		case Function::level: {
			double percent = value.as_real();
			if (!(percent >= 0.0 && percent <= 100.0)) {
				Log::warn(kTag, "command of '%s' refused: a level takes 0..100 %%", datapoint.name());
				return false;
			}
			return true;
		}
		case Function::color_temperature: {
			uint32_t kelvin = value.as_unsigned();
			if (kelvin < 16 || kelvin > 1000000) {
				Log::warn(kTag, "command of '%s' refused: a colour temperature takes 16..1000000 K", datapoint.name());
				return false;
			}
			return true;
		}
		default:
			Log::warn(kTag, "command of '%s' refused: %s has no command", datapoint.name(),
				function_info(point.function).key);
			return false;
	}
}

bool ZigbeeAdapter::send_command(const ZigbeeDevice& device, const ZigbeePoint& point, const Datapoint& datapoint,
	const Value& value) {
	bool sent = false;
	uint16_t sent_cluster = point.command_cluster;
	uint16_t sent_command = 0;
	// The ZCL command ids for the monitor: Move to Level (with On/Off) 0x04, On 0x01, Off 0x00, Move to
	// Color Temperature 0x0A.
	switch (point.function) {
		case Function::on_off:
			if (point.command_cluster == cluster::kLevelControl) {
				sent_command = 0x04;
				sent = driver_.move_to_level(device.ieee, device.short_addr, point.endpoint, value.as_bool() ? 254 : 0,
					kTransitionDs);
			} else {
				sent_command = value.as_bool() ? 0x01 : 0x00;
				sent = driver_.on_off(device.ieee, device.short_addr, point.endpoint,
					value.as_bool() ? OnOffCommand::on : OnOffCommand::off);
			}
			break;
		case Function::level: {
			long level = std::lround(value.as_real() * 254.0 / 100.0);
			sent_command = 0x04;
			sent = driver_.move_to_level(device.ieee, device.short_addr, point.endpoint, static_cast<uint8_t>(level),
				kTransitionDs);
			break;
		}
		case Function::color_temperature: {
			long mireds = std::lround(1e6 / static_cast<double>(value.as_unsigned()));
			sent_command = 0x0A;
			sent = driver_.move_to_color_temperature(device.ieee, device.short_addr, point.endpoint,
				static_cast<uint16_t>(mireds), kTransitionDs);
			break;
		}
		default:
			break;
	}
	if (!sent) {
		Log::warn(kTag, "command of '%s' refused: the Zigbee stack did not take it", datapoint.name());
		return false;
	}
	traffic(ZigbeeTraffic::Kind::command, device, point.endpoint, sent_cluster, sent_command, datapoint.id(), &value);
	return true;
}

size_t ZigbeeAdapter::commands_held() const {
	size_t result = 0;
	for (size_t i = 0; i < model_.size(); ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		result += dp->pending() && dp->technology() == this ? 1 : 0;
	}
	return result;
}

void ZigbeeAdapter::send_held() {
	held_timer_ = TimerHandle();
	if (!network_.up) {
		return;
	}
	for (size_t id = held_next_; id < model_.size(); ++id) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(id));
		if (!dp->pending() || dp->technology() != this) {
			continue;
		}
		held_next_ = static_cast<uint16_t>(id + 1);
		model_.set_pending(*this, dp->id(), false);
		long index = index_of(dp->id());
		const ZigbeeDevice* device = index >= 0 ? database_.find(points_[index].ieee) : nullptr;
		char text[48];
		dp->value().format(text, sizeof(text));
		if (device == nullptr || device->short_addr == kNoShortAddress || !points_[index].commandable()) {
			Log::warn(kTag, "dropping the held command %s for '%s': the device has not joined", text, dp->name());
		} else if (send_command(*device, points_[index], *dp, dp->value())) {
			Log::info(kTag, "sent the held command %s for '%s'", text, dp->name());
		}
		held_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kHeldSpacingMs,
			Callback::member<ZigbeeAdapter, &ZigbeeAdapter::send_held>(this, "zigbee.send_held"));
		return;
	}
	hold_logged_ = false;
}

bool ZigbeeAdapter::read(const Datapoint& datapoint) {
	long index = index_of(datapoint.id());
	if (index < 0) {
		Log::error(kTag, "read of '%s' refused: not a Zigbee datapoint", datapoint.name());
		return false;
	}
	const ZigbeePoint& point = points_[index];
	if ((point.flags() & DatapointFlags::kReadOnStart) == 0) {
		// Client commands (buttons) have no attribute; a multistate input reports events, which a read
		// would repeat (the adapter does not read it on start either).
		Log::warn(kTag, "read of '%s' refused: %s has no attribute value to read", datapoint.name(),
			function_info(point.function).key);
		return false;
	}
	const ZigbeeDevice* device = database_.find(point.ieee);
	if (device == nullptr || device->short_addr == kNoShortAddress) {
		Log::warn(kTag, "read of '%s' refused: the device has not joined", datapoint.name());
		return false;
	}
	if (device->left) {
		Log::warn(kTag, "read of '%s' refused: the device left the network", datapoint.name());
		return false;
	}
	uint16_t attribute = point.attribute();
	if (!driver_.read_attributes(device->ieee, device->short_addr, point.endpoint, point.cluster(), &attribute, 1)) {
		Log::warn(kTag, "read of '%s' refused: the Zigbee stack did not take it", datapoint.name());
		return false;
	}
	return true;
}

// ---- JSON -----------------------------------------------------------------------------------

uint32_t ZigbeeAdapter::permit_join_s() const {
	uint64_t now = clock_.monotonic_ms();
	return network_.permit_until_ms > now ? static_cast<uint32_t>((network_.permit_until_ms - now + 999) / 1000) : 0;
}

void ZigbeeAdapter::notify(ZigbeeEvent::Kind kind, Ieee ieee, uint32_t permit_join_s) {
	++generation_;
	if (events_ == nullptr) {
		return;
	}
	ZigbeeEvent event;
	event.kind = kind;
	event.ieee = ieee;
	event.permit_join_s = permit_join_s;
	events_->zigbee_event(event);
}

const char* zigbee_event_name(ZigbeeEvent::Kind kind) {
	switch (kind) {
		case ZigbeeEvent::Kind::network: return "network";
		case ZigbeeEvent::Kind::permit: return "permit";
		case ZigbeeEvent::Kind::joined: return "joined";
		case ZigbeeEvent::Kind::interviewing: return "interviewing";
		case ZigbeeEvent::Kind::interviewed: return "interviewed";
		case ZigbeeEvent::Kind::interview_failed: return "interview_failed";
		case ZigbeeEvent::Kind::interview_incomplete: return "interview_incomplete";
		case ZigbeeEvent::Kind::left: return "left";
		case ZigbeeEvent::Kind::removed: return "removed";
		case ZigbeeEvent::Kind::changed: return "changed";
	}
	return "changed";
}

void ZigbeeAdapter::write_network_json(JsonWriter& json) const {
	json.member("up", network_.up);
	if (network_.up) {
		char ext[kIeeeText];
		format_ieee(network_.extended_pan, ext);
		json.member("pan_id", static_cast<uint32_t>(network_.pan_id));
		json.member("channel", static_cast<uint32_t>(network_.channel));
		json.member("extended_pan_id", ext);
	}
	json.member("permit_join_s", permit_join_s());
}

void ZigbeeAdapter::write_device_json(JsonWriter& json, size_t index) const {
	const ZigbeeDevice& device = database_.at(index);
	char ieee[kIeeeText];
	format_ieee(device.ieee, ieee);
	json.begin_object();
	json.member("ieee", ieee);
	if (device.short_addr != kNoShortAddress) {
		json.member("short", static_cast<uint32_t>(device.short_addr));
	}
	json.member("manufacturer", device.manufacturer);
	json.member("model", device.model);
	json.member("label", device.label);
	json.member("interviewed", device.interviewed);
	json.member("stale", device.stale);
	json.member("left", device.left);
	json.member("interviewing", interview_.active && interview_.ieee == device.ieee);
	if (device.incomplete()) {
		write_missing_json(json, device);
		char why[96];
		json.member("incomplete", missing_text(device, why, sizeof(why)));
	}
	if (device.last_seen_s > 0) {
		json.member("last_seen", static_cast<int64_t>(device.last_seen_s));
	}
	if (device.seen_ms != 0) {
		json.member("seen_ago_s", (clock_.monotonic_ms() - device.seen_ms) / 1000);
	}
	write_radio_json(json, device);
	if (rejoin_.ieee == device.ieee) {
		json.key("rejoin").begin_object()
			.member("requested_ago_s", (clock_.monotonic_ms() - rejoin_.since_ms) / 1000)
			.member("sent", static_cast<uint32_t>(rejoin_.sends))
			.member("accepted", rejoin_.accepted)
			.end_object();
	}
	json.key("datapoints").begin_array();
	for (size_t p = 0; loaded_ && p < point_count_; ++p) {
		if (points_[p].ieee == device.ieee && ids_[p] != kNoId) {
			const Datapoint* dp = model_.get(ids_[p]);
			if (dp != nullptr) {
				json.string(dp->name());
			}
		}
	}
	json.end_array();
	json.end_object();
}

void ZigbeeAdapter::write_radio_json(JsonWriter& json, const ZigbeeDevice& device) const {
	if (device.has(ZigbeeDevice::kRadioKnown)) {
		json.member("lqi", static_cast<uint32_t>(device.lqi));
		json.member("rssi", static_cast<int32_t>(device.rssi));
	}
	if (device.has(ZigbeeDevice::kRouter)) {
		if (device.has(ZigbeeDevice::kCoordinatorLink)) {
			json.member("coordinator_lqi", static_cast<uint32_t>(device.coordinator_lqi));
		}
		return; // a router has no parent
	}
	if (device.has(ZigbeeDevice::kChild)) {
		json.member("parent", "coordinator");
		json.member("timeout_s", device.child_timeout_s);
		json.member("expires_in_s", device.child_expires_ms / 1000);
		return;
	}
	if (device.parent_short == kNoShortAddress || device.parent_short == 0x0000) {
		return; // no parent known (a child of the coordinator is in its child table)
	}
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& parent = database_.at(i);
		if (parent.short_addr == device.parent_short && parent.ieee != device.ieee) {
			char ieee[kIeeeText];
			format_ieee(parent.ieee, ieee);
			json.member("parent", ieee);
			if (parent.label[0] != '\0') {
				json.member("parent_label", parent.label);
			}
			if (device.has(ZigbeeDevice::kParentListed)) {
				json.member("parent_lqi", static_cast<uint32_t>(device.parent_lqi));
				json.member("depth", static_cast<uint32_t>(device.depth));
			}
			return;
		}
	}
}

void ZigbeeAdapter::write_device_export_json(JsonWriter& json, size_t index) const {
	const ZigbeeDevice& device = database_.at(index);
	char ieee[kIeeeText];
	format_ieee(device.ieee, ieee);
	json.begin_object();
	json.member("ieee", ieee);
	json.member("manufacturer", device.manufacturer);
	json.member("model", device.model);
	json.member("label", device.label);
	json.member("interviewed", device.interviewed);
	if (device.incomplete()) {
		write_missing_json(json, device);
	}
	json.key("endpoints").begin_array();
	for (size_t e = 0; e < device.endpoint_count; ++e) {
		const ZigbeeEndpoint& endpoint = device.endpoints[e];
		json.begin_object();
		json.member("id", static_cast<uint32_t>(endpoint.id));
		json.member("profile", static_cast<uint32_t>(endpoint.profile));
		json.member("device", static_cast<uint32_t>(endpoint.device));
		json.key("in").begin_array();
		for (size_t c = 0; c < endpoint.in_count; ++c) {
			json.number(static_cast<uint32_t>(endpoint.in[c]));
		}
		json.end_array();
		json.key("out").begin_array();
		for (size_t c = 0; c < endpoint.out_count; ++c) {
			json.number(static_cast<uint32_t>(endpoint.out[c]));
		}
		json.end_array();
		if (endpoint.zone_type != ZigbeeEndpoint::kNoZoneType) {
			json.member("zone_type", static_cast<uint32_t>(endpoint.zone_type));
		}
		json.end_object();
	}
	json.end_array();
	json.key("functions").begin_array();
	// Every function the mapping derives, bound or not (spec/zigbee.md "Export").
	ZigbeePoint points[kMaxDevicePoints];
	size_t count = map_device(device, points, kMaxDevicePoints);
	if (count > kMaxDevicePoints) {
		count = kMaxDevicePoints;
	}
	for (size_t i = 0; i < count; ++i) {
		const ZigbeePoint& point = points[i];
		const FunctionInfo& info = function_info(point.function);
		// The effective reporting: the binding's (default or override), else the default.
		Reporting reporting = point.reporting;
		reporting_in(overrides_, override_count_, device, point.endpoint, point.function, reporting);
		char generic[Datapoint::kMaxName + 1];
		json.begin_object();
		json.member("name", point_name(point, generic, sizeof(generic)));
		json.member("function", info.key);
		json.member("endpoint", static_cast<uint32_t>(point.endpoint));
		json.member("cluster", static_cast<uint32_t>(info.cluster));
		if (info.attribute != 0xFFFF) { // action, a client binding, has no attribute
			json.member("attribute", static_cast<uint32_t>(info.attribute));
		}
		json.member("type", value_type_name(point.type()));
		if (unit_of(point.function) != nullptr) {
			json.member("unit", unit_of(point.function));
		}
		json.member("writable", point.command_cluster != cluster::kNone);
		if (configures_reporting(point.function)) {
			json.key("reporting").begin_object();
			json.member("min", static_cast<uint32_t>(reporting.min_s));
			json.member("max", static_cast<uint32_t>(reporting.max_s));
			double change = 0;
			if (change_to_unit(point.function, reporting.change, change)) {
				json.member("change", change);
			}
			json.end_object();
		}
		// The name the loaded model gives it (a binding of the image, or the generic one).
		for (size_t p = 0; loaded_ && p < point_count_; ++p) {
			if (points_[p].ieee == device.ieee && points_[p].endpoint == point.endpoint &&
				points_[p].function == point.function && ids_[p] != kNoId) {
				const Datapoint* dp = model_.get(ids_[p]);
				if (dp != nullptr) {
					json.member("datapoint", dp->name());
				}
				break;
			}
		}
		json.end_object();
	}
	json.end_array();
	json.end_object();
}

void ZigbeeAdapter::write_json(JsonWriter& json) const {
	json.begin_object();
	json.key("network").begin_object();
	write_network_json(json);
	json.end_object();
	json.key("devices").begin_array();
	for (size_t i = 0; i < database_.size(); ++i) {
		write_device_json(json, i);
	}
	json.end_array();
	json.end_object();
}

} // namespace zigbee
} // namespace hc
