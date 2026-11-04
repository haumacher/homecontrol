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

	/// The application asks for the current value of a datapoint this technology owns (a rule's
	/// `read` action, spec/rules.md 7.12): the technology fetches it from its side (KNX: a
	/// GroupValueRead) and the answer arrives later as an ordinary report. Return true when the
	/// request is accepted (sent or queued), false to refuse (log the reason). The default refuses:
	/// a technology that cannot fetch a value. Like command(), it must not call back into the model
	/// synchronously.
	virtual bool read(const Datapoint& datapoint) {
		(void)datapoint;
		return false;
	}

	/// Whether the technology holds a command while it is not connected (the KNX tunnel is down, the
	/// Zigbee network is not up) instead of refusing it, and sends it once it connects
	/// (homecontrol #72, spec/rules.md 7.3). Such a technology marks the datapoint with
	/// DatapointModel::set_pending() while it holds the value. A load keeps the mark of a datapoint it
	/// keeps the value of (DatapointModel::load() with `keep_values`); bind() drops it when the new
	/// owner does not hold commands. The default: it does not.
	virtual bool holds_commands() const { return false; }

protected:
	~Technology() = default;
};

} // namespace hc

#endif // HC_TECHNOLOGY_H
