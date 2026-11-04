/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_MODEL_VIEW_H
#define HC_IMAGE_MODEL_VIEW_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/image/image.h"
#include "hc/image/loader.h"

namespace hc {
namespace image {

/// What the device UI needs of a loaded image beyond the components' own state (GET /api/model and
/// GET /api/pages, spec/api.md): the locations, the functions with their roles, the datapoints' labels,
/// units and bindings, the control pages, the step of each rule parameter and whether a schedule's switch is adjustable. Built by
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
		const char* label;   ///< JSON text.
		const char* kind;    ///< "light", "dimmer", ...
		const char* unit;    ///< "" for none.
		const char* presses; ///< JSON object text of a button's `presses` (1.4), nullptr for none.
		const Role* roles;
		int16_t room;        ///< Index into the locations, -1 for none.
		uint8_t scene;       ///< 1..64 for a scene, else 0.
		uint8_t role_count;
	};

	struct DatapointInfo {
		const char* label;   ///< JSON text, nullptr for none.
		const char* unit;    ///< nullptr for none.
		const char* binding; ///< JSON object text of the technology members ({"knx": {...}}), nullptr for none.
	};

	/// The control pages (`ui.pages`, spec/runtime-image.md 3.9) in flat tables: the pages, their
	/// sections, their elements, the series and reference lines of the charts, each in image order;
	/// an entry names its owner by index. Labels are JSON text (nullptr for none).
	struct Page {
		const char* id;
		const char* label;
		const char* icon; ///< nullptr for none.
		uint16_t sections;
	};

	struct Section {
		const char* label;
		uint16_t page;
		uint16_t elements;
	};

	enum class ElementKind : uint8_t { function, datapoint, chart };

	struct Element {
		const char* label;
		uint16_t section;
		uint16_t target; ///< The function's index, the datapoint's id, or the chart's index.
		ElementKind kind;
		bool read_only;
	};

	struct Chart {
		const char* label;
		double min[2]; ///< Per axis (left, right): the fixed minimum where `fixed` says so.
		double max[2];
		uint8_t fixed;   ///< Bits: 1 left min, 2 left max, 4 right min, 8 right max.
		uint8_t width;   ///< 1..4 columns, 0 for "full".
		uint8_t height;  ///< 2 or 4 tile rows.
		uint8_t range;   ///< Index of chart_range_name().
		uint8_t presets; ///< Bit per range.
	};

	struct Series {
		const char* label;
		uint16_t element;
		uint16_t datapoint;
		uint8_t axis; ///< 0 left, 1 right.
		Consolidation consolidation; ///< The image's, or the default for the datapoint's type.
	};

	struct Reference {
		const char* label;
		double value;       ///< The constant.
		uint16_t element;
		uint16_t datapoint; ///< When not `constant`: the datapoint whose current value it draws.
		uint8_t axis;
		bool constant;
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

	size_t page_count() const { return page_count_; }
	const Page& page(size_t i) const { return pages_[i]; }
	size_t section_count() const { return section_count_; }
	const Section& section(size_t i) const { return sections_[i]; }
	size_t element_count() const { return element_count_; }
	const Element& element(size_t i) const { return elements_[i]; }
	size_t chart_count() const { return chart_count_; }
	const Chart& chart(size_t i) const { return charts_[i]; }
	size_t series_count() const { return series_count_; }
	const Series& series(size_t i) const { return series_[i]; }
	size_t reference_count() const { return reference_count_; }
	const Reference& reference(size_t i) const { return references_[i]; }

	/// The declared step of a rule parameter; 0 when the image gives none.
	double parameter_step(const char* rule, const char* parameter) const;
	/// Whether the image lets the inhabitant switch schedule entry `id` (true when it does not say).
	bool schedule_adjustable(const char* id) const;

	/// Heap bytes of the view.
	size_t bytes() const { return arena_.bytes(); }
	/// Of those, the bytes the tables use.
	size_t used() const { return arena_.used(); }

private:
	bool build_pages(const JsonSpan& pages, const DatapointModel& model);

	Arena arena_;
	Location* locations_ = nullptr;
	size_t location_count_ = 0;
	Function* functions_ = nullptr;
	size_t function_count_ = 0;
	DatapointInfo* datapoints_ = nullptr;
	size_t datapoint_count_ = 0;
	Page* pages_ = nullptr;
	size_t page_count_ = 0;
	Section* sections_ = nullptr;
	size_t section_count_ = 0;
	Element* elements_ = nullptr;
	size_t element_count_ = 0;
	Chart* charts_ = nullptr;
	size_t chart_count_ = 0;
	Series* series_ = nullptr;
	size_t series_count_ = 0;
	Reference* references_ = nullptr;
	size_t reference_count_ = 0;
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
