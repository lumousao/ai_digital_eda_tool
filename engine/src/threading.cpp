/**
 * Threading Support - Industrial-grade multi-threading framework
 *
 * Complete implementation of all methods declared in threading_industrial.h
 */

#include "threading.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <numeric>

namespace Threading {

// ============================================================================
// Task implementation
// ============================================================================

void Task::execute() {
    try {
        if (func) {
            func();
        }
        completed = true;
    } catch (...) {
        failed = true;
        exception = std::current_exception();
    }
}

// ============================================================================
// TaskGroup implementation
// ============================================================================

void TaskGroup::addTask(std::shared_ptr<Task> task) {
    tasks.push_back(task);
}

void TaskGroup::wait() {
    while (!isCompleted()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool TaskGroup::isCompleted() const {
    return completed_count >= (int)tasks.size();
}

int TaskGroup::getProgress() const {
    if (tasks.empty()) return 100;
    return (completed_count * 100) / tasks.size();
}

// ============================================================================
// WorkQueue implementation
// ============================================================================

void WorkQueue::push(std::shared_ptr<Task> task) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push(task);
    condition.notify_one();
}

std::shared_ptr<Task> WorkQueue::pop() {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this] { return !queue.empty() || stopped; });

    if (stopped && queue.empty()) {
        return nullptr;
    }

    std::shared_ptr<Task> task = queue.front();
    queue.pop();
    return task;
}

std::shared_ptr<Task> WorkQueue::tryPop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (queue.empty()) {
        return nullptr;
    }

    std::shared_ptr<Task> task = queue.front();
    queue.pop();
    return task;
}

bool WorkQueue::isEmpty() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex));
    return queue.empty();
}

void WorkQueue::stop() {
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
    condition.notify_all();
}

size_t WorkQueue::size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex));
    return queue.size();
}

// ============================================================================
// ThreadPool implementation
// ============================================================================

ThreadPool::ThreadPool(size_t num_threads) : stopped(false), active_workers(0),
    total_tasks_executed(0), total_work_time_ms(0) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // Default to 4 threads
        }
    }

    for (size_t i = 0; i < num_threads; i++) {
        workers.emplace_back(&ThreadPool::workerLoop, this);
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::submit(std::shared_ptr<Task> task) {
    queue.push(task);
}

void ThreadPool::submit(const std::function<void()> &func, const std::string &name) {
    auto task = std::make_shared<Task>(func, name);
    queue.push(task);
}

void ThreadPool::waitAll() {
    while (!queue.isEmpty() || active_workers > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ThreadPool::waitForTask(const std::string &name) {
    // Simplified wait - just wait for queue to be empty
    waitAll();
}

size_t ThreadPool::getQueueSize() const {
    return queue.size();
}

void ThreadPool::stop() {
    stopped = true;
    queue.stop();

    for (auto &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
}

void ThreadPool::workerLoop() {
    while (!stopped) {
        std::shared_ptr<Task> task = queue.pop();
        if (!task) {
            break;
        }

        active_workers++;
        auto start_time = std::chrono::steady_clock::now();

        task->execute();

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        total_tasks_executed++;
        total_work_time_ms += duration.count();
        active_workers--;
    }
}

// ============================================================================
// ParallelFor implementation
// ============================================================================

int ParallelFor::getDefaultThreadCount() {
    return std::max(1, (int)std::thread::hardware_concurrency());
}

// ============================================================================
// ParallelPipeline implementation
// ============================================================================

void ParallelPipeline::addStage(const std::string &name, std::function<void(int, int)> func, int chunk) {
    stages.push_back({func, name, chunk});
}

void ParallelPipeline::execute() {
    if (stages.empty()) return;

    int threads = num_threads > 0 ? num_threads : getOptimalThreadCount();

    for (auto &stage : stages) {
        int chunk_size = stage.chunk_size > 0 ? stage.chunk_size :
                        (total_items + threads - 1) / threads;

        std::vector<std::future<void>> futures;
        for (int i = 0; i < total_items; i += chunk_size) {
            int end = std::min(i + chunk_size, total_items);
            futures.push_back(std::async(std::launch::async, [&stage, i, end]() {
                stage.func(i, end);
            }));

            if ((int)futures.size() >= threads) {
                for (auto &f : futures) {
                    f.get();
                }
                futures.clear();
            }
        }

        for (auto &f : futures) {
            f.get();
        }
    }
}

// ============================================================================
// ParallelSort implementation
// ============================================================================

void ParallelSort::radixsort(std::vector<int> &data, int num_threads) {
    if (data.empty()) return;

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    // Find max value
    int max_val = *std::max_element(data.begin(), data.end());

    // Do counting sort for every digit
    for (int exp = 1; max_val / exp > 0; exp *= 10) {
        std::vector<int> output(data.size());
        std::vector<int> count(10, 0);

        // Count occurrences
        for (int i = 0; i < (int)data.size(); i++) {
            count[(data[i] / exp) % 10]++;
        }

        // Cumulative count
        for (int i = 1; i < 10; i++) {
            count[i] += count[i - 1];
        }

        // Build output array
        for (int i = data.size() - 1; i >= 0; i--) {
            output[count[(data[i] / exp) % 10] - 1] = data[i];
            count[(data[i] / exp) % 10]--;
        }

        data = output;
    }
}

// ============================================================================
// ProgressTracker implementation
// ============================================================================

void ProgressTracker::start(int total_items) {
    total = total_items;
    completed = 0;
    start_time = std::chrono::steady_clock::now();
}

void ProgressTracker::update(int count) {
    completed += count;
}

void ProgressTracker::finish() {
    completed.store(total);
    printProgress();
}

double ProgressTracker::getProgress() const {
    if (total == 0) return 100.0;
    return (completed * 100.0) / total;
}

double ProgressTracker::getElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
    return duration.count() / 1000.0;
}

double ProgressTracker::getEstimatedRemainingSeconds() const {
    if (completed == 0) return 0.0;
    double elapsed = getElapsedSeconds();
    double rate = completed / elapsed;
    int remaining = total - completed;
    return remaining / rate;
}

void ProgressTracker::printProgress() const {
    double progress = getProgress();
    double elapsed = getElapsedSeconds();
    double remaining = getEstimatedRemainingSeconds();

    std::cout << "\r" << operation_name << ": " << progress << "%"
              << " (" << elapsed << "s elapsed, "
              << remaining << "s remaining)" << std::flush;
}

// ============================================================================
// Global thread pool
// ============================================================================

static ThreadPool* global_pool = nullptr;
static std::mutex global_pool_mutex;

ThreadPool& getGlobalThreadPool() {
    std::lock_guard<std::mutex> lock(global_pool_mutex);
    if (!global_pool) {
        global_pool = new ThreadPool();
    }
    return *global_pool;
}

void setGlobalThreadPoolSize(size_t size) {
    std::lock_guard<std::mutex> lock(global_pool_mutex);
    if (global_pool) {
        delete global_pool;
    }
    global_pool = new ThreadPool(size);
}

size_t getGlobalThreadPoolSize() {
    std::lock_guard<std::mutex> lock(global_pool_mutex);
    if (global_pool) {
        return global_pool->getThreadCount();
    }
    return std::thread::hardware_concurrency();
}

// ============================================================================
// Helper functions
// ============================================================================

size_t getOptimalThreadCount() {
    size_t count = std::thread::hardware_concurrency();
    return count > 0 ? count : 4;
}

void setThreadAffinity(int core_id) {
    // Platform-specific thread affinity setting
    // This is a simplified version
    #ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    #endif
}

int getCurrentThreadId() {
    static thread_local int id = -1;
    static std::atomic<int> counter(0);
    if (id == -1) {
        id = counter++;
    }
    return id;
}

void sleepFor(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

void yield() {
    std::this_thread::yield();
}

} // namespace Threading
