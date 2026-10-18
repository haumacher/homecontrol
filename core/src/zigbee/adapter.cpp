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

const char* step_name(uint8_t step) {
	switch (step) {
		case 0: return "active endpoints";
		case 1: return "simple descriptor";
		case 2: return "manufacturer and model";
		case 3: return "IAS zone type";
	}
	return "?";
}

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
	loop_.cancel(interview_.timer);
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
		database_.at(i).refresh = database_.at(i).interviewed;
	}
	check_timer_ = loop_.schedule_periodic(start_ms_ + kCheckPeriodMs, kCheckPeriodMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::check_stale>(this));
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
	delete[] old_table;
	delete[] old_names;
	Log::info(kTag, "%u binding(s) from the runtime image", static_cast<unsigned>(count));
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
			saved[i] = dp != nullptr ? Saved{dp->value(), dp->quality()} : Saved{Value(), Quality::unknown};
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
	// Carry the values over: a good value is reported again, a stale one restored as stale.
	if (saved != nullptr) {
		for (size_t i = 0; i < count; ++i) {
			for (size_t j = 0; j < point_count_; ++j) {
				if (!same_point(table[i], points_[j]) || saved[j].quality == Quality::unknown) {
					continue;
				}
				if (saved[j].quality == Quality::good) {
					model_.report(*this, static_cast<uint16_t>(i), saved[j].value);
				} else {
					model_.restore(static_cast<uint16_t>(i), saved[j].value);
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
		if (device != nullptr && device->interviewed && !device->refresh) {
			device->refresh = true;
			refresh = true;
		}
	}
	// Adopt: the bindings, the points and their ids.
	free_overrides();
	overrides_ = next->overrides;
	override_names_ = next->override_names;
	override_count_ = next->override_count;
	next->overrides = nullptr;
	next->override_names = nullptr;
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
		if (overrides_[i].ieee == device.ieee) {
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
	if (device->short_addr == kNoShortAddress || !driver_.leave(ieee, device->short_addr)) {
		Log::warn(kTag, "%s not asked to leave (no address or the stack refused): it rejoins as a new device "
			"when it is heard again", text);
	}
	if (interview_.active && interview_.ieee == ieee) {
		end_interview();
	}
	database_.remove(ieee);
	save_database();
	Log::info(kTag, "%s removed", text);
	if (started_) {
		rebuild(nullptr);
	}
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
	if (interview_.active) {
		Log::info(kTag, "interview queued behind the running one");
		return true;
	}
	start_pending_interview();
	return true;
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
		device.refresh = device.interviewed;
		char text[64];
		Log::info(kTag, "%s heard again: reading its datapoints", device_text(device, text, sizeof(text)));
		schedule_refresh();
	}
	if (!device.interviewed && !device.interview_pending &&
		(device.interview_ms == 0 || now - device.interview_ms >= kInterviewRetryMs) &&
		!(interview_.active && interview_.ieee == device.ieee)) {
		device.interview_pending = true;
		start_pending_interview();
	}
}

uint32_t ZigbeeAdapter::stale_timeout_s(Ieee ieee) const {
	uint32_t longest = 0;
	for (size_t i = 0; i < point_count_; ++i) {
		if (points_[i].ieee == ieee) {
			uint32_t max_s = function_info(points_[i].function).max_s;
			if (max_s > longest) {
				longest = max_s;
			}
		}
	}
	return longest * kStaleFactor;
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
	uint64_t now = clock_.monotonic_ms();
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
		if (loaded_) {
			for (size_t p = 0; p < point_count_; ++p) {
				if (points_[p].ieee == device.ieee && ids_[p] != kNoId) {
					model_.report_stale(*this, ids_[p]);
				}
			}
		}
	}
}

void ZigbeeAdapter::schedule_refresh() {
	if (refresh_timer_.valid()) {
		return;
	}
	refresh_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kRefreshSpacingMs,
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::refresh_due>(this));
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
		auto readable = [&](const ZigbeePoint& p) {
			return p.ieee == device->ieee && (p.flags() & DatapointFlags::kReadOnStart) != 0;
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
	device.interview_ms = clock_.monotonic_ms();
	loop_.cancel(interview_.timer);
	interview_.active = true;
	interview_.ieee = device.ieee;
	interview_.step = Step::active_endpoints;
	interview_.attempts = 0;
	interview_.index = 0;
	interview_.queried_count = 0;
	interview_.result = ZigbeeDevice();
	interview_.result.ieee = device.ieee;
	interview_.timer = TimerHandle();
	char text[64];
	Log::info(kTag, "interviewing %s (short 0x%04x)", device_text(device, text, sizeof(text)),
		static_cast<unsigned>(device.short_addr));
	send_step();
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
		Callback::member<ZigbeeAdapter, &ZigbeeAdapter::step_timeout>(this));
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
		case Step::simple_descriptor:
			Log::warn(kTag, "no simple descriptor for endpoint %u: skipped",
				static_cast<unsigned>(interview_.queried[interview_.index]));
			++interview_.index;
			next_simple_descriptor();
			break;
		case Step::basic:
			Log::warn(kTag, "no manufacturer and model from the Basic cluster: the device stays unnamed");
			start_zone(0);
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
		fail_interview("no endpoint of the Home Automation or Light Link profile");
		return;
	}
	start_basic();
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
	start_pending_interview();
}

void ZigbeeAdapter::finish_interview() {
	end_interview();
	ZigbeeDevice* device = database_.find(interview_.ieee);
	if (device == nullptr) {
		start_pending_interview();
		return;
	}
	const ZigbeeDevice& result = interview_.result;
	std::memcpy(device->manufacturer, result.manufacturer, sizeof(device->manufacturer));
	std::memcpy(device->model, result.model, sizeof(device->model));
	device->endpoint_count = result.endpoint_count;
	for (size_t i = 0; i < result.endpoint_count; ++i) {
		device->endpoints[i] = result.endpoints[i];
	}
	device->interviewed = true;
	save_database();
	if (started_) {
		rebuild(nullptr);
	}
	ZigbeePoint mapped[kMaxDevicePoints];
	size_t points = device_points(*device, mapped, kMaxDevicePoints);
	char ieee[kIeeeText];
	format_ieee(device->ieee, ieee);
	Log::info(kTag, "device %s interviewed: \"%s\" \"%s\", %u endpoint(s), %u datapoint(s)", ieee, device->manufacturer,
		device->model, static_cast<unsigned>(device->endpoint_count), static_cast<unsigned>(points));
	setup_reporting(*device);
	device->refresh = true;
	schedule_refresh();
	start_pending_interview();
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
		if (info.max_s > 0) {
			ReportingConfig config{info.attribute, info.zcl_type, info.min_s, info.max_s, info.change};
			driver_.configure_reporting(device.ieee, device.short_addr, point.endpoint, info.cluster, config);
			++configured;
		}
	}
	char text[64];
	Log::info(kTag, "%s: %u binding(s) to the coordinator, reporting of %u attribute(s) configured",
		device_text(device, text, sizeof(text)), static_cast<unsigned>(bound_count), static_cast<unsigned>(configured));
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
	} else {
		network_.permit_until_ms = 0;
		Log::warn(kTag, "network down");
	}
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
}

void ZigbeeAdapter::device_announced(NodeAddress address) {
	if (address.ieee == 0) {
		Log::warn(kTag, "device announce without IEEE address (short 0x%04x) ignored",
			static_cast<unsigned>(address.short_addr));
		return;
	}
	bool known = database_.find(address.ieee) != nullptr;
	ZigbeeDevice* device = resolve(address, true);
	if (device == nullptr) {
		return;
	}
	char text[64];
	if (known) {
		Log::info(kTag, "%s announced itself (short 0x%04x)", device_text(*device, text, sizeof(text)),
			static_cast<unsigned>(address.short_addr));
	}
	if (device->left && device->interviewed) {
		// It left (a factory reset) and joined again: its endpoints may have changed.
		device->interview_pending = true;
	}
	device->left = false;
	touch(*device);
	start_pending_interview();
}

void ZigbeeAdapter::device_left(NodeAddress address) {
	ZigbeeDevice* device = resolve(address, false);
	if (device == nullptr) {
		Log::info(kTag, "an unknown device (short 0x%04x) left the network", static_cast<unsigned>(address.short_addr));
		return;
	}
	device->left = true;
	char text[64];
	Log::info(kTag, "%s left the network (it stays in the database; `zigbee remove` deletes it)",
		device_text(*device, text, sizeof(text)));
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
		start_zone(0);
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
	bool sent = false;
	uint16_t sent_cluster = point.command_cluster;
	uint16_t sent_command = 0;
	// The ZCL command ids for the monitor: Move to Level (with On/Off) 0x04, On 0x01, Off 0x00, Move to
	// Color Temperature 0x0A.
	switch (point.function) {
		case Function::on_off:
			if (point.command_cluster == cluster::kLevelControl) {
				sent_command = 0x04;
				sent = driver_.move_to_level(device->ieee, device->short_addr, point.endpoint, value.as_bool() ? 254 : 0,
					kTransitionDs);
			} else {
				sent_command = value.as_bool() ? 0x01 : 0x00;
				sent = driver_.on_off(device->ieee, device->short_addr, point.endpoint,
					value.as_bool() ? OnOffCommand::on : OnOffCommand::off);
			}
			break;
		case Function::level: {
			double percent = value.as_real();
			if (!(percent >= 0.0 && percent <= 100.0)) {
				Log::warn(kTag, "command of '%s' refused: a level takes 0..100 %%", datapoint.name());
				return false;
			}
			long level = std::lround(percent * 254.0 / 100.0);
			sent_command = 0x04;
			sent = driver_.move_to_level(device->ieee, device->short_addr, point.endpoint, static_cast<uint8_t>(level),
				kTransitionDs);
			break;
		}
		case Function::color_temperature: {
			uint32_t kelvin = value.as_unsigned();
			if (kelvin < 16 || kelvin > 1000000) {
				Log::warn(kTag, "command of '%s' refused: a colour temperature takes 16..1000000 K", datapoint.name());
				return false;
			}
			long mireds = std::lround(1e6 / static_cast<double>(kelvin));
			sent_command = 0x0A;
			sent = driver_.move_to_color_temperature(device->ieee, device->short_addr, point.endpoint,
				static_cast<uint16_t>(mireds), kTransitionDs);
			break;
		}
		default:
			Log::warn(kTag, "command of '%s' refused: %s has no command", datapoint.name(),
				function_info(point.function).key);
			return false;
	}
	if (!sent) {
		Log::warn(kTag, "command of '%s' refused: the Zigbee stack did not take it", datapoint.name());
		return false;
	}
	traffic(ZigbeeTraffic::Kind::command, *device, point.endpoint, sent_cluster, sent_command, datapoint.id(), &value);
	return true;
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

void ZigbeeAdapter::write_json(JsonWriter& json) const {
	uint64_t now = clock_.monotonic_ms();
	json.begin_object();
	json.key("network").begin_object();
	json.member("up", network_.up);
	if (network_.up) {
		char ext[kIeeeText];
		format_ieee(network_.extended_pan, ext);
		json.member("pan_id", static_cast<uint32_t>(network_.pan_id));
		json.member("channel", static_cast<uint32_t>(network_.channel));
		json.member("extended_pan_id", ext);
	}
	uint32_t open_s = network_.permit_until_ms > now ? static_cast<uint32_t>((network_.permit_until_ms - now + 999) / 1000) : 0;
	json.member("permit_join_s", open_s);
	json.end_object();
	json.key("devices").begin_array();
	for (size_t i = 0; i < database_.size(); ++i) {
		const ZigbeeDevice& device = database_.at(i);
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
		if (device.last_seen_s > 0) {
			json.member("last_seen", static_cast<int64_t>(device.last_seen_s));
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
	json.end_array();
	json.end_object();
}

} // namespace zigbee
} // namespace hc
