/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_MODEL_VIEW_H
#define HC_IMAGE_MODEL_VIEW_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/image/loader.h"

namespace hc {
namespace image {

/// What the device UI needs of a loaded image beyond the components' own state (GET /api/model,
/// spec/api.md): the locations, the functions with their roles, the datapoints' labels, units and
/// bindings, the step of each rule parameter and whether a schedule's switch is adjustable. Built by
/// ImageService from the validated text after every successful load (read-only afterwards).
///
/// Labels and bindings are kept as JSON text copied verbatim from the image (a label may be an
/// object of language tags, which the UI resolves); everything was validated by the Loader, so the
/// copies are valid JSON.
///
/// Memory: one arena per build, allocated at load and freed by the next build or clear().
class ModelView {
public:
	struct Location {
		const char* id;
		const char* label; ///< JSON text: a string or an object of language tags.
		const char* kind;  ///< "" when the image names none.
		int16_t parent;    ///< Index into the locations, -1 for none.
	};

	struct Role {
		const char* name;   ///< "command", "state", "levelCommand", ...
		uint16_t datapoint; ///< Id in the DatapointModel.
	};

	struct Function {
		const char* id;
		const char* label; ///< JSON text.
		const char* kind;  ///< "light", "dimmer", ...
		int16_t room;      ///< Index into the locations, -1 for none.
		const char* unit;  ///< "" for none.
		uint8_t scene;     ///< 1..64 for a scene, else 0.
		const Role* roles;
		uint8_t role_count;
	};

	struct DatapointInfo {
		const char* label;   ///< JSON text, nullptr for none.
		const char* unit;    ///< nullptr for none.
		const char* binding; ///< JSON object text of the technology members ({"knx": {...}}), nullptr for none.
	};

	struct ParameterStep {
		const char* rule;
		const char* parameter;
		double step;
	};

	ModelView() = default;
	ModelView(const ModelView&) = delete;
	ModelView& operator=(const ModelView&) = delete;

	/// Rebuilds the view from the validated image `text` against the loaded `model`. False (the view
	/// is empty then, `no_memory()` true) when the heap is exhausted; the image still runs.
	bool build(const char* text, size_t length, const DatapointModel& model);

	/// Empties the view and frees its memory.
	void clear();

	/// Changes with every build and clear (a client paging through /api/model restarts on a change).
	uint32_t generation() const { return generation_; }
	bool no_memory() const { return no_memory_; }

	size_t location_count() const { return location_count_; }
	const Location& location(size_t i) const { return locations_[i]; }
	size_t function_count() const { return function_count_; }
	const Function& function(size_t i) const { return functions_[i]; }
	/// The datapoints of the model at build time (ids 0 ..).
	size_t datapoint_count() const { return datapoint_count_; }
	const DatapointInfo& datapoint(size_t id) const { return datapoints_[id]; }

	/// The declared step of a rule parameter; 0 when the image gives none.
	double parameter_step(const char* rule, const char* parameter) const;
	/// Whether the image lets the inhabitant switch schedule entry `id` (true when it does not say).
	bool schedule_adjustable(const char* id) const;

	/// Heap bytes of the view.
	size_t bytes() const { return arena_.bytes(); }

private:
	Arena arena_;
	Location* locations_ = nullptr;
	size_t location_count_ = 0;
	Function* functions_ = nullptr;
	size_t function_count_ = 0;
	DatapointInfo* datapoints_ = nullptr;
	size_t datapoint_count_ = 0;
	ParameterStep* steps_ = nullptr;
	size_t step_count_ = 0;
	const char** fixed_schedules_ = nullptr; ///< Ids of the entries with enabledAdjustable false.
	size_t fixed_schedule_count_ = 0;
	uint32_t generation_ = 0;
	bool no_memory_ = false;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_MODEL_VIEW_H
