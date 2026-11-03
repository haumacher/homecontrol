/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A Worker the test drives step by step (homecontrol #68): run() only records the job; run_job() is
// the worker's thread doing it, deliver() the loop running `done`.
#ifndef HC_TEST_FAKE_WORKER_H
#define HC_TEST_FAKE_WORKER_H

#include "hc/platform/worker.h"

namespace hc_test {

class ManualWorker : public hc::Worker {
public:
	bool run(hc::Callback job, hc::Callback done, const char** error) override {
		if (refuse != nullptr) {
			*error = refuse;
			return false;
		}
		if (pending) {
			*error = "a job runs already";
			return false;
		}
		job_ = job;
		done_ = done;
		pending = true;
		ran = false;
		++runs;
		return true;
	}

	bool busy() const override { return pending; }

	/// The worker's thread: runs the job (once).
	void run_job() {
		if (pending && !ran) {
			ran = true;
			job_();
		}
	}

	/// The loop: `done` (after the job).
	void deliver() {
		run_job();
		if (pending) {
			pending = false;
			hc::Callback done = done_;
			done();
		}
	}

	/// Both.
	void step() { deliver(); }

	const char* refuse = nullptr; ///< Set: run() refuses with this reason.
	bool pending = false;
	bool ran = false;
	int runs = 0;

private:
	hc::Callback job_;
	hc::Callback done_;
};

} // namespace hc_test

#endif // HC_TEST_FAKE_WORKER_H
