/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TECHNOLOGY_H
#define HC_TECHNOLOGY_H

#include "hc/datapoint.h"

namespace hc {

/// A technology adapter: translates between a bus or a service (KNX, Zigbee, weather) and the
/// datapoint model. The rules and the UI never see it, only datapoints (ROADMAP item 2).
///
/// An adapter owns the datapoints bound to it (DatapointModel::bind()). It reports what its side
/// says with DatapointModel::report() / report_stale(), and receives the commands the application
/// gives its transmitting datapoints through command(). It is a core concept: the KNX adapter
/// (issue #7) implements it; the tests use a loopback fake.
///
/// Threading: called on the event loop's thread; the adapter reports on it too.
class Technology {
public:
	/// Short name for the log ("knx", "zigbee").
	virtual const char* name() const = 0;

	/// The application commands `value` (already converted to the datapoint's type) for a
	/// datapoint this technology owns and that transmits. Return true when the value is accepted
	/// for transmission (queued or sent): the model then stores it. Return false to refuse (log
	/// the reason): the model keeps the old value and the command fails with
	/// DatapointStatus::refused. Called before the model changes; it must not call back into the
	/// model synchronously (post() to report later).
	virtual bool command(const Datapoint& datapoint, const Value& value) = 0;

protected:
	~Technology() = default;
};

} // namespace hc

#endif // HC_TECHNOLOGY_H
