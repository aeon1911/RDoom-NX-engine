/*
** nx_jobs.cpp
** Worker threads for background asset preparation.
**
** On the Switch the engine's main thread is pinned to core 0 and the
** workers run on cores 1 and 2 with a lower priority than everything else,
** so they only use CPU time that would otherwise be wasted.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <pthread.h>
#include <stdlib.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <deque>
#include "nx_jobs.h"
#include "c_cvars.h"

#ifdef __SWITCH__
void I_NXSetupWorkerThread(int core);
void I_NXWorkerYield();
// One worker on core 2 by default: core 0 runs the game, core 1 is left to
// the render worker, audio and music threads (a 2nd worker would go there).
CVAR(Int, nx_workers, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#else
CVAR(Int, nx_workers, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
#endif

namespace
{
	// Deliberately never destroyed (only its threads are stopped at exit):
	// destroying a condition variable somebody may still wait on can hang.
	// Note: libnx has no pthread_detach (std::thread::detach throws there),
	// so the workers are joinable threads that get joined on exit.
	struct Pool
	{
		std::mutex mutex;
		std::condition_variable work, idle;
		// things needed right now go first, then likely-needed-soon, then prefetching
		std::deque<std::function<void()>> now, high, low;
		bool empty() const { return now.empty() && high.empty() && low.empty(); }
		int running = 0;
		int threads = 0;
		bool started = false;
		bool quit = false;
		std::vector<pthread_t> handles;
	};

	Pool &GetPool()
	{
		static Pool *pool = new Pool;
		return *pool;
	}

	void WorkerMain(int index)
	{
#ifdef __SWITCH__
		I_NXSetupWorkerThread((index % 2) == 0 ? 2 : 1);
#endif
		Pool &p = GetPool();
		for (;;)
		{
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(p.mutex);
				p.work.wait(lock, [&p] { return !p.empty() || p.quit; });
				if (p.quit) return;
				auto &q = !p.now.empty() ? p.now : !p.high.empty() ? p.high : p.low;
				job = std::move(q.front());
				q.pop_front();
				p.running++;
			}
			try
			{
				job();
			}
			catch (...)
			{
				// a broken asset must not take the game down
			}
			job = nullptr;
			{
				std::lock_guard<std::mutex> lock(p.mutex);
				p.running--;
				if (p.running == 0 && p.empty()) p.idle.notify_all();
			}
#ifdef __SWITCH__
			// threads of the same priority only get the core when the running
			// one gives it up: let audio/music/render threads in between jobs
			I_NXWorkerYield();
#endif
		}
	}

	void *WorkerEntry(void *arg)
	{
		WorkerMain((int)(intptr_t)arg);
		return nullptr;
	}

	// stops and joins the workers (registered with atexit when they start)
	void Shutdown()
	{
		Pool &p = GetPool();
		std::vector<pthread_t> handles;
		{
			std::lock_guard<std::mutex> lock(p.mutex);
			if (p.quit) return;
			p.quit = true;
			p.now.clear();
			p.high.clear();
			p.low.clear();
			handles.swap(p.handles);
		}
		p.work.notify_all();
		for (auto &h : handles) pthread_join(h, nullptr);
		std::lock_guard<std::mutex> lock(p.mutex);
		p.threads = 0;
		p.running = 0;
		p.idle.notify_all();
	}

	// call with the mutex held
	void StartWorkers(Pool &p)
	{
		p.started = true;
		int n = nx_workers;
		if (n < 1) n = 1;
		if (n > 4) n = 4;
		for (int i = 0; i < n; i++)
		{
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			// image and sound decoders can use a fair amount of stack
			pthread_attr_setstacksize(&attr, 1024 * 1024);
			pthread_t h;
			if (pthread_create(&h, &attr, WorkerEntry, (void *)(intptr_t)i) == 0)
			{
				p.handles.push_back(h);
				p.threads++;
			}
			pthread_attr_destroy(&attr);
		}
		if (p.threads > 0) atexit(Shutdown);
	}
}

void NXJobs_Shutdown()
{
	Shutdown();
}

bool NXJobs_Enabled()
{
	if (nx_workers <= 0) return false;
	Pool &p = GetPool();
	if (!p.started)
	{
		std::lock_guard<std::mutex> lock(p.mutex);
		if (!p.started && !p.quit) StartWorkers(p);
	}
	// no worker could be started (or we're exiting): callers do the work themselves
	return !p.quit && p.threads > 0;
}

void NXJobs_Push(std::function<void()> job, int priority)
{
	Pool &p = GetPool();
	{
		std::unique_lock<std::mutex> lock(p.mutex);
		if (!p.started && !p.quit) StartWorkers(p);
		if (p.threads == 0 || p.quit)
		{
			lock.unlock();
			try { job(); } catch (...) {}
			return;
		}
		if (priority >= NXJOB_NOW) p.now.push_back(std::move(job));
		else if (priority == NXJOB_HIGH) p.high.push_back(std::move(job));
		else p.low.push_back(std::move(job));
	}
	p.work.notify_one();
}

void NXJobs_WaitAll()
{
	Pool &p = GetPool();
	std::unique_lock<std::mutex> lock(p.mutex);
	if (p.threads == 0) return;
	p.idle.wait(lock, [&p] { return p.running == 0 && p.empty(); });
}
