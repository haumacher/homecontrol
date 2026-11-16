/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_MODEL_VIEW_H
#define HC_IMAGE_MODEL_VIEW_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/image/binary.h"
#include "hc/image/image.h"

namespace hc {

class JsonWriter;

namespace image {

/// What the UI reads of the loaded image beyond the components (GET /api/model, spec/api.md): the
/// locations, the functions with their roles, the datapoints' labels, units and bindings, written
/// straight from the image in its slot (issue #87: no copy in RAM). The control pages are the
/// image's `ui` section, served as they are (GET /api/pages).
class ModelView {
public:
	ModelView() = default;
	ModelView(const ModelView&) = delete;
	ModelView& operator=(const ModelView&) = delete;

	/// Points the view at the loaded image (which stays mapped while the view is used).
	void build(const bin::View& view, const DatapointModel& model);
	void clear();

	/// Changes with every build: a client paging through GET /api/model starts over.
	uint32_t generation() const { return generation_; }

	size_t location_count() const { return view_.count(bin::kLocations); }
	/// `{"id", "label", "kind"?, "parent"?}`.
	void write_location(JsonWriter& json, size_t i) const;
	size_t function_count() const { return view_.count(bin::kFunctions); }
	/// `{"id", "label", "kind", "room"?, "unit"?, "scene"?, "presses"?, "datapoints": {role: id}}`.
	void write_function(JsonWriter& json, size_t i) const;

	/// The image's datapoints (the model's first ids).
	size_t datapoint_count() const { return view_.count(bin::kDatapoints); }
	/// The label reference of image datapoint `id` (0: none).
	uint32_t datapoint_label(size_t id) const;
	/// Its unit, nullptr for none.
	const char* datapoint_unit(size_t id) const;
	/// Writes `"binding": {...}` (the technology members as the source gave them: `knx`, `zigbee`,
	/// `virtual`) when the datapoint has one; false otherwise.
	bool write_binding(JsonWriter& json, size_t id) const;
	/// The DPT of a KNX datapoint; false for any other.
	bool knx_dpt(size_t id, Dpt& out) const;

	/// A label as the image gives it (a string or a language map; "" for none).
	void write_label(JsonWriter& json, uint32_t ref) const;
	/// Whether the image lets the inhabitant switch schedule entry `id` (true when it does not say).
	bool schedule_adjustable(const char* id) const;

	const bin::View& image() const { return view_; }

private:
	bin::View view_;
	uint32_t generation_ = 0;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_MODEL_VIEW_H
