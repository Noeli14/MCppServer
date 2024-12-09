#include "thread_pool.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

thread_pool::thread_pool(size_t numThreads) : stop(false) {
    for(size_t i = 0;i<numThreads;++i)
        workers.emplace_back(
            [this]
            {
                while(true)
                {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock,
                            [this]{ return this->stop.load() || !this->tasks.empty(); });
                        if(this->stop.load() && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    task();
                }
            }
        );
	
    for(size_t i = 0;i<numThreads;++i)
	{
		std::string threadName = "PoolWkr:";
		threadName += std::to_string(i);
		set_thread_name(workers[i], threadName.data());
	}
}

// Destructor joins all threads
thread_pool::~thread_pool()
{
    stop.store(true);
    condition.notify_all();
    for(std::thread &worker: workers)
        worker.join();
}

void set_thread_name(std::thread& thread, const char* threadName)
{
#if defined(__linux__)
    const auto handle = thread.native_handle();
	pthread_setname_np(handle, threadName);
#endif
}
