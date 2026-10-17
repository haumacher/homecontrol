/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_DYNAMIC_SPECS_H
#define HC_IMAGE_DYNAMIC_SPECS_H

#include <cstddef>

#include "hc/datapoint.h"
#include "hc/technology.h"
#include "hc/zigbee/cluster_mapping.h"

namespace hc {
namespace image {

/// One datapoint a dynamic spec provider needs in the model right now (doc/image.md "Dynamic
/// datapoints"): a joined Zigbee device's function, later a discovered service's value.
struct DynamicSpec {
	/// Name, type and flags. The name follows the model's rules and stays valid until the provider's
	/// next dynamic_specs(), model_loaded() or model_cleared().
	DatapointSpec spec{nullptr, ValueType::none, 0};
	/// The technology the image service binds the datapoint to after the load (nullptr: the provider
	/// binds it itself in model_loaded()).
	Technology* technology = nullptr;
	/// For the UI ("°C", "%"); nullptr when none. The model does not carry units or labels yet (the
	/// image's own `unit` and `label` are validated and not kept either); they are part of the spec
	/// so a provider states them once the UI reads them from the model.
	const char* unit = nullptr;
	const char* label = nullptr;
};

/// The technology bindings of a runtime image that a provider takes over: the datapoints the image
/// declares and the provider serves (they stay the image's datapoints, at the image's positions).
struct ImageBindings {
	/// The `zigbee` bindings of the image's datapoints (spec/zigbee.md); names are the datapoints'.
	const zigbee::ZigbeeOverride* zigbee = nullptr;
	size_t zigbee_count = 0;
};

class DynamicSpecProvider;

/// Told when a provider's specs changed (the image service, which reloads the model).
class DynamicSpecListener {
public:
	virtual void dynamic_specs_changed(DynamicSpecProvider& provider) = 0;

protected:
	~DynamicSpecListener() = default;
};

/// A component that needs datapoints of its own next to the runtime image's (doc/image.md "Dynamic
/// datapoints"). The image service composes the model from the image's datapoints followed by every
/// provider's specs; a name that appears twice refuses the composition with the provider named.
///
/// Protocol, all on the event loop's thread:
///  1. dynamic_specs(bindings): the specs the provider needs given these image bindings (those of
///     the image being applied, or of the loaded one). Nothing is committed: an apply may still be
///     refused before the model changes.
///  2. model_loaded(bindings): the model now holds the image's datapoints and the specs of the last
///     dynamic_specs() call (with value, quality and update time kept by name and type). The
///     provider resolves its datapoints by name and binds those without a technology.
///  3. model_cleared(): the model was emptied or replaced without the provider's specs; the
///     provider's datapoint ids are invalid.
///  When its specs change (a device joined, was removed), the provider calls notify_specs_changed();
///  the service reloads the model later (rate-limited, never from within the call).
class DynamicSpecProvider {
public:
	/// Short name for refusals and the log ("zigbee").
	virtual const char* provider_name() const = 0;

	/// Sets `specs` and `count` to the datapoints needed with `bindings`; false with `error` when the
	/// provider cannot build them (memory, bindings it refuses).
	virtual bool dynamic_specs(const ImageBindings& bindings, const DynamicSpec*& specs, size_t& count,
		const char** error) = 0;

	/// The model was loaded with the specs of the last dynamic_specs() call and `bindings`.
	virtual void model_loaded(const ImageBindings& bindings) = 0;

	/// The model no longer holds the provider's datapoints.
	virtual void model_cleared() = 0;

	/// Set by the image service when the provider is registered (nullptr: not registered).
	void set_spec_listener(DynamicSpecListener* listener) { spec_listener_ = listener; }
	/// Whether an image service composes the model (the provider must not load it itself).
	bool has_spec_listener() const { return spec_listener_ != nullptr; }

protected:
	~DynamicSpecProvider() = default;

	void notify_specs_changed() {
		if (spec_listener_ != nullptr) {
			spec_listener_->dynamic_specs_changed(*this);
		}
	}

private:
	DynamicSpecListener* spec_listener_ = nullptr;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_DYNAMIC_SPECS_H
