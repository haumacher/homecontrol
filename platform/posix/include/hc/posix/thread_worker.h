/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_THREAD_WORKER_H
#define HC_POSIX_THREAD_WORKER_H

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "hc/platform/worker.h"
#include "hc/posix/web_server.h"

namespace hc {
namespace posix {

/// The core's Worker on POSIX (homecontrol #68): each job runs on a thread of its own; `done` is
/// brought onto the loop's thread through the LoopExecutor (the thread waits until the loop ran it).
/// The destructor waits for a running job's thread to end: destroy the worker before the objects the job uses,
/// and stop the executor first when the loop no longer runs (as main() does), or the thread would
/// wait for the loop for ever.
class ThreadWorker : public Worker {
public:
	explicit ThreadWorker(LoopExecutor& executor) : executor_(executor) {}
	~ThreadWorker();
	ThreadWorker(const ThreadWorker&) = delete;
	ThreadWorker& operator=(const ThreadWorker&) = delete;

	bool run(Callback job, Callback done, const char** error) override;
	bool busy() const override { return busy_.load(); }

private:
	LoopExecutor& executor_;
	std::atomic<bool> busy_{false};
	/// Threads that have not ended yet (a job's thread ends after its `done` ran).
	std::mutex mutex_;
	std::condition_variable ended_;
	int alive_ = 0;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_THREAD_WORKER_H
