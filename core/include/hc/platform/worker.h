/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_WORKER_H
#define HC_PLATFORM_WORKER_H

#include "hc/event_loop.h"

namespace hc {

/// Runs one job off the event loop (homecontrol #68: the runtime image's store, which writes to the
/// flash for a few hundred milliseconds and needs nothing of the loop's state): a short-lived task on
/// the ESP (doc/core.md "Off the loop"), a thread on POSIX, a stepped fake in the tests.
///
/// The job runs on the worker's thread and must touch only what may be used there: its own data, the
/// BlobStore on blobs nobody writes meanwhile (doc/persistence.md "Threads"), the log. When it has
/// returned, `done` runs on the event loop's thread, never within run(); from then on the loop may
/// read what the job wrote. One job at a time.
///
/// Every call is made on the event loop's thread.
class Worker {
public:
	/// Starts `job`; `done` follows on the loop. False when a job is in flight or the platform cannot
	/// start one (no memory for the task); `error` (static text) says why, and neither callback runs.
	virtual bool run(Callback job, Callback done, const char** error) = 0;

	/// Whether a job is in flight (its `done` has not run yet).
	virtual bool busy() const = 0;

protected:
	~Worker() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WORKER_H
