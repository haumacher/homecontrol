/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/thread_worker.h"

#include <thread>

namespace hc {
namespace posix {

ThreadWorker::~ThreadWorker() {
	std::unique_lock<std::mutex> lock(mutex_);
	ended_.wait(lock, [this] { return alive_ == 0; });
}

bool ThreadWorker::run(Callback job, Callback done, const char** error) {
	if (busy_.load()) {
		*error = "a job runs already";
		return false;
	}
	busy_.store(true);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		++alive_;
	}
	// Detached: `done` may start the next job while this thread still returns from the executor.
	std::thread([this, job, done] {
		job();
		// On the loop; false when the executor stopped (shutdown): then nobody waits for `done`.
		executor_.run([this, done] {
			busy_.store(false);
			done();
		});
		std::lock_guard<std::mutex> lock(mutex_);
		--alive_;
		ended_.notify_all();
	}).detach();
	return true;
}

} // namespace posix
} // namespace hc
