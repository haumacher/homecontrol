/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/model_view.h"

#include <cstdio>
#include <cstring>

#include "hc/json.h"
#include "hc/knx/address.h"
#include "hc/zigbee/zcl.h"

namespace hc {
namespace image {

using namespace bin;

namespace {

constexpr const char* kLocationKinds[] = {"", "building", "floor", "room", "area", "outside"};
constexpr const char* kFunctionKinds[] = {"light", "dimmer", "blind", "switch", "sensor", "climate", "scene", "button"};
constexpr const char* kPressKinds[] = {"press", "on", "off", "up", "down", "release"};

void write_address(JsonWriter& json, const View& view, uint16_t index) {
	char text[knx::GroupAddress::kMaxText];
	knx::GroupAddress{view.at<KnxAddressRec>(kKnxAddresses, index).address}.format(text, sizeof(text));
	json.string(text);
}

} // namespace

void ModelView::build(const View& view, const DatapointModel& model) {
	(void)model;
	view_ = view;
	++generation_;
}

void ModelView::clear() {
	view_.close();
	++generation_;
}

void ModelView::write_label(JsonWriter& json, uint32_t ref) const {
	if (ref == 0) {
		json.string("");
	} else {
		bin::write_label(json, view_, ref);
	}
}

void ModelView::write_location(JsonWriter& json, size_t i) const {
	const LocationRec& l = view_.at<LocationRec>(kLocations, i);
	json.begin_object().member("id", view_.str(l.id)).key("label");
	write_label(json, l.label);
	if (l.kind > 0 && l.kind <= 5) {
		json.member("kind", kLocationKinds[l.kind]);
	}
	if (l.parent != kNone) {
		json.member("parent", view_.str(view_.at<LocationRec>(kLocations, l.parent).id));
	}
	json.end_object();
}

void ModelView::write_function(JsonWriter& json, size_t i) const {
	const FunctionRec& f = view_.at<FunctionRec>(kFunctions, i);
	json.begin_object().member("id", view_.str(f.id)).key("label");
	write_label(json, f.label);
	json.member("kind", f.kind <= 7 ? kFunctionKinds[f.kind] : "?");
	if (f.room != kNone) {
		json.member("room", view_.str(view_.at<LocationRec>(kLocations, f.room).id));
	}
	if (f.unit != 0) {
		json.member("unit", view_.str(f.unit));
	}
	if (f.scene > 0) {
		json.member("scene", static_cast<uint32_t>(f.scene));
	}
	bool presses = false;
	for (uint8_t bits : f.presses) {
		presses = presses || bits != 0;
	}
	if (presses) {
		json.key("presses").begin_object();
		for (size_t k = 0; k < 6; ++k) {
			if (f.presses[k] == 0) {
				continue;
			}
			json.key(kPressKinds[k]).begin_array();
			for (int e = 0; e < 6; ++e) {
				if ((f.presses[k] & (1u << e)) != 0) {
					json.number(static_cast<int32_t>(e));
				}
			}
			json.end_array();
		}
		json.end_object();
	}
	json.key("datapoints").begin_object();
	for (size_t r = 0; r < f.role_count; ++r) {
		const RoleRec& role = view_.at<RoleRec>(kRoles, f.first_role + r);
		json.member(view_.str(role.name), static_cast<uint32_t>(role.datapoint));
	}
	json.end_object().end_object();
}

uint32_t ModelView::datapoint_label(size_t id) const {
	return id < datapoint_count() ? view_.at<DatapointRec>(kDatapoints, id).label : 0;
}

const char* ModelView::datapoint_unit(size_t id) const {
	return id < datapoint_count() ? view_.opt(view_.at<DatapointRec>(kDatapoints, id).unit) : nullptr;
}

bool ModelView::knx_dpt(size_t id, Dpt& out) const {
	if (id >= datapoint_count()) {
		return false;
	}
	const DatapointRec& d = view_.at<DatapointRec>(kDatapoints, id);
	if (d.owner != kOwnerKnx) {
		return false;
	}
	const KnxBindingRec& b = view_.at<KnxBindingRec>(kKnxBindings, d.owner_index);
	out.main = b.dpt_main;
	out.sub = b.dpt_sub;
	return true;
}

bool ModelView::write_binding(JsonWriter& json, size_t id) const {
	if (id >= datapoint_count()) {
		return false;
	}
	const DatapointRec& d = view_.at<DatapointRec>(kDatapoints, id);
	if (d.owner == kOwnerKnx) {
		const KnxBindingRec& b = view_.at<KnxBindingRec>(kKnxBindings, d.owner_index);
		char dpt[16];
		std::snprintf(dpt, sizeof(dpt), "%u.%03u", static_cast<unsigned>(b.dpt_main), static_cast<unsigned>(b.dpt_sub));
		json.key("binding").begin_object().key("knx").begin_object().member("dpt", dpt);
		if (b.send != kNone) {
			json.key("send");
			write_address(json, view_, b.send);
		}
		if (b.listen_count > 0) {
			json.key("listen").begin_array();
			for (size_t k = 0; k < b.listen_count; ++k) {
				write_address(json, view_, view_.at<uint16_t>(kKnxListen, b.first_listen + k));
			}
			json.end_array();
		}
		json.end_object().end_object();
		return true;
	}
	if (d.owner == kOwnerZigbee) {
		const ZigbeeBindingRec& z = view_.at<ZigbeeBindingRec>(kZigbeeBindings, d.owner_index);
		char ieee[zigbee::kIeeeText];
		zigbee::format_ieee(z.ieee, ieee);
		json.key("binding").begin_object().key("zigbee").begin_object()
			.member("ieee", ieee)
			.member("endpoint", static_cast<uint32_t>(z.endpoint))
			.member("cluster", static_cast<uint32_t>(z.cluster));
		if ((z.flags & 0x01) != 0) {
			json.member("client", true);
		} else {
			json.member("attribute", static_cast<uint32_t>(z.attribute));
		}
		if (z.command != 0xFFFF) {
			json.member("command", static_cast<uint32_t>(z.command));
		}
		if ((z.flags & 0x0E) != 0) {
			json.key("reporting").begin_object();
			if ((z.flags & 0x02) != 0) {
				json.member("min", static_cast<uint32_t>(z.report_min));
			}
			if ((z.flags & 0x04) != 0) {
				json.member("max", static_cast<uint32_t>(z.report_max));
			}
			if ((z.flags & 0x08) != 0) {
				json.member("change", z.report_change);
			}
			json.end_object();
		}
		json.end_object().end_object();
		return true;
	}
	if ((d.flags & kDatapointVirtual) != 0) {
		json.key("binding").begin_object().key("virtual").begin_object().end_object().end_object();
		return true;
	}
	return false;
}

bool ModelView::schedule_adjustable(const char* id) const {
	for (size_t i = 0; i < view_.count(kSchedules); ++i) {
		const ScheduleRec& s = view_.at<ScheduleRec>(kSchedules, i);
		if (std::strcmp(view_.str(s.id), id) == 0) {
			return (s.flags & 0x02) != 0;
		}
	}
	return true;
}

} // namespace image
} // namespace hc
