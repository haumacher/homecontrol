/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The POSIX Worker (homecontrol #68): the job on a thread of its own, `done` on the loop's thread.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "doctest.h"
#include "hc/posix/thread_worker.h"

namespace {

struct Probe {
	std::thread::id loop;
	std::atomic<bool> job_ran{false};
	std::atomic<bool> release{false};
	std::thread::id job_thread;
	std::thread::id done_thread;
	bool done = false;
	bool busy_in_done = true;

	static void job(void* context) {
		Probe& p = *static_cast<Probe*>(context);
		p.job_thread = std::this_thread::get_id();
		while (!p.release.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		p.job_ran.store(true);
	}
};

hc::posix::ThreadWorker* g_worker = nullptr;

void on_done(void* context) {
	Probe& p = *static_cast<Probe*>(context);
	p.done_thread = std::this_thread::get_id();
	p.done = true;
	p.busy_in_done = g_worker->busy();
}

} // namespace

TEST_CASE("ThreadWorker: the job runs off the loop while the loop goes on; done follows on the loop's thread") {
	hc::posix::LoopExecutor executor;
	Probe probe;
	probe.loop = std::this_thread::get_id();
	{
		hc::posix::ThreadWorker worker(executor);
		g_worker = &worker;
		const char* error = nullptr;
		REQUIRE(worker.run(hc::Callback{&Probe::job, &probe}, hc::Callback{&on_done, &probe}, &error));
		CHECK(worker.busy());
		// A second job while one runs is refused with the reason.
		CHECK_FALSE(worker.run(hc::Callback{&Probe::job, &probe}, hc::Callback{&on_done, &probe}, &error));
		CHECK(std::string(error) == "a job runs already");
		// The loop runs while the job waits: nothing is done yet.
		executor.wait(20);
		CHECK_FALSE(probe.done);
		probe.release.store(true);
		auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!probe.done && std::chrono::steady_clock::now() < until) {
			executor.wait(20);
		}
		REQUIRE(probe.done);
		CHECK(probe.job_ran.load());
		CHECK((probe.job_thread != probe.loop));
		CHECK((probe.done_thread == probe.loop));
		CHECK_FALSE(probe.busy_in_done); // done may start the next job
		CHECK_FALSE(worker.busy());
		// The next job starts at once.
		probe.done = false;
		REQUIRE(worker.run(hc::Callback{&Probe::job, &probe}, hc::Callback{&on_done, &probe}, &error));
		until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!probe.done && std::chrono::steady_clock::now() < until) {
			executor.wait(20);
		}
		CHECK(probe.done);
	} // the destructor waits for the thread's end
	g_worker = nullptr;
}
