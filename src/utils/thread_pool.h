#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

class thread_pool {
public:
    // Constructor: Initializes the pool with the given number of threads
    thread_pool(size_t numThreads);

    // Enqueue a task into the pool
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::result_of_t<F(Args...)>>;

    // Destructor: Joins all threads
    ~thread_pool();

private:
    // Worker threads
    std::vector<std::thread> workers;

    // Task queue
    std::queue<std::function<void()>> tasks;

    // Synchronization
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

// Add new work item to the pool
template<class F, class... Args>
auto thread_pool::enqueue(F&& f, Args&&... args)
    -> std::future<std::result_of_t<F(Args...)>>
{
    using return_type = std::result_of_t<F(Args...)>;

    auto task = std::make_shared< std::packaged_task<return_type()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task->get_future();
    {
        std::lock_guard lock(queue_mutex);

        // Don't allow enqueueing after stopping the pool
        if(stop.load())
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task](){ (*task)(); });
    }
    condition.notify_one();
    return res;
}

void set_thread_name(std::thread& thread, const char* threadName);

#endif //THREADPOOL_H
