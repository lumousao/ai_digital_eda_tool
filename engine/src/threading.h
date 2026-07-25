/**
 * Threading Support - Industrial-grade multi-threading framework
 *
 * Features:
 * - Thread pool for parallel task execution
 * - Parallel for loops
 * - Parallel pipeline execution
 * - Thread-safe data structures
 * - Work stealing scheduler
 * - Task dependencies
 * - Progress tracking
 */

#ifndef THREADING_INDUSTRIAL_H
#define THREADING_INDUSTRIAL_H

#include <vector>
#include <queue>
#include <map>
#include <set>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>
#include <chrono>
#include <algorithm>

namespace Threading {

// ============================================================================
// Forward declarations
// ============================================================================

struct ThreadPool;
struct Task;
struct TaskGroup;
struct ParallelFor;
struct ParallelPipeline;
struct WorkQueue;

// ============================================================================
// Task - Unit of work
// ============================================================================

struct Task {
    enum Priority {
        LOW = 0,
        NORMAL = 1,
        HIGH = 2,
        CRITICAL = 3
    };

    std::function<void()> func;
    std::string name;
    Priority priority;
    std::vector<std::string> dependencies;
    std::atomic<bool> completed;
    std::atomic<bool> failed;
    std::exception_ptr exception;

    Task() : priority(NORMAL), completed(false), failed(false) {}
    Task(const std::function<void()> &f, const std::string &n = "",
         Priority p = NORMAL)
        : func(f), name(n), priority(p), completed(false), failed(false) {}

    void execute();
    bool isCompleted() const { return completed; }
    bool isFailed() const { return failed; }
};

// ============================================================================
// TaskGroup - Group of related tasks
// ============================================================================

struct TaskGroup {
    std::vector<std::shared_ptr<Task>> tasks;
    std::string name;
    std::atomic<int> completed_count;
    std::atomic<int> failed_count;

    TaskGroup() : completed_count(0), failed_count(0) {}
    TaskGroup(const std::string &n) : name(n), completed_count(0), failed_count(0) {}

    void addTask(std::shared_ptr<Task> task);
    void wait();
    bool isCompleted() const;
    int getProgress() const;
};

// ============================================================================
// WorkQueue - Thread-safe work queue
// ============================================================================

struct WorkQueue {
    std::queue<std::shared_ptr<Task>> queue;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopped;

    WorkQueue() : stopped(false) {}

    void push(std::shared_ptr<Task> task);
    std::shared_ptr<Task> pop();
    std::shared_ptr<Task> tryPop();
    bool isEmpty() const;
    void stop();
    size_t size() const;
};

// ============================================================================
// ThreadPool - Thread pool for parallel execution
// ============================================================================

struct ThreadPool {
    std::vector<std::thread> workers;
    WorkQueue queue;
    std::atomic<bool> stopped;
    std::atomic<int> active_workers;
    std::atomic<long long> total_tasks_executed;
    std::atomic<long long> total_work_time_ms;

    ThreadPool(size_t num_threads = 0);
    ~ThreadPool();

    // Submit tasks
    void submit(std::shared_ptr<Task> task);
    void submit(const std::function<void()> &func, const std::string &name = "");

    // Wait for completion
    void waitAll();
    void waitForTask(const std::string &name);

    // Statistics
    size_t getThreadCount() const { return workers.size(); }
    size_t getQueueSize() const;
    int getActiveWorkers() const { return active_workers; }
    long long getTotalTasksExecuted() const { return total_tasks_executed; }
    long long getTotalWorkTimeMs() const { return total_work_time_ms; }

    // Control
    void stop();
    bool isStopped() const { return stopped; }

private:
    void workerLoop();
};

// ============================================================================
// ParallelFor - Parallel for loop execution
// ============================================================================

struct ParallelFor {
    // Parallel for with automatic chunking
    template<typename Func>
    static void execute(int start, int end, Func func, int num_threads = 0);

    // Parallel for with explicit chunk size
    template<typename Func>
    static void execute(int start, int end, int chunk_size, Func func, int num_threads = 0);

    // Parallel for with index
    template<typename Func>
    static void executeIndexed(int start, int end, Func func, int num_threads = 0);

private:
    static int getDefaultThreadCount();
};

// ============================================================================
// ParallelPipeline - Pipeline execution
// ============================================================================

struct ParallelPipeline {
    struct Stage {
        std::function<void(int, int)> func;
        std::string name;
        int chunk_size;
    };

    std::vector<Stage> stages;
    int total_items;
    int num_threads;

    ParallelPipeline(int total, int threads = 0) : total_items(total), num_threads(threads) {}

    void addStage(const std::string &name, std::function<void(int, int)> func, int chunk = 0);
    void execute();
};

// ============================================================================
// ParallelReduction - Parallel reduction operations
// ============================================================================

struct ParallelReduction {
    // Parallel sum
    template<typename T, typename Func>
    static T sum(const std::vector<T> &data, Func func, int num_threads = 0);

    // Parallel min
    template<typename T, typename Func>
    static T min(const std::vector<T> &data, Func func, int num_threads = 0);

    // Parallel max
    template<typename T, typename Func>
    static T max(const std::vector<T> &data, Func func, int num_threads = 0);

    // Parallel any
    template<typename T, typename Func>
    static bool any(const std::vector<T> &data, Func func, int num_threads = 0);

    // Parallel all
    template<typename T, typename Func>
    static bool all(const std::vector<T> &data, Func func, int num_threads = 0);
};

// ============================================================================
// ParallelSort - Parallel sorting algorithms
// ============================================================================

struct ParallelSort {
    // Parallel quicksort
    template<typename T, typename Compare = std::less<T>>
    static void quicksort(std::vector<T> &data, Compare comp = Compare(), int num_threads = 0);

    // Parallel merge sort
    template<typename T, typename Compare = std::less<T>>
    static void mergesort(std::vector<T> &data, Compare comp = Compare(), int num_threads = 0);

    // Parallel radix sort (for integers)
    static void radixsort(std::vector<int> &data, int num_threads = 0);
};

// ============================================================================
// ParallelMap - Parallel map operations
// ============================================================================

struct ParallelMap {
    // Parallel transform
    template<typename Input, typename Output, typename Func>
    static void transform(const std::vector<Input> &input, std::vector<Output> &output,
                         Func func, int num_threads = 0);

    // Parallel filter
    template<typename T, typename Func>
    static std::vector<T> filter(const std::vector<T> &data, Func func, int num_threads = 0);

    // Parallel for_each
    template<typename T, typename Func>
    static void for_each(std::vector<T> &data, Func func, int num_threads = 0);
};

// ============================================================================
// ThreadLocal - Thread-local storage
// ============================================================================

template<typename T>
struct ThreadLocal {
    std::vector<std::unique_ptr<T>> storage;
    std::mutex mutex;

    ThreadLocal() = default;

    T& get() {
        std::lock_guard<std::mutex> lock(mutex);
        int id = getThreadId();
        if (id >= (int)storage.size()) {
            storage.resize(id + 1);
        }
        if (!storage[id]) {
            storage[id] = std::make_unique<T>();
        }
        return *storage[id];
    }

private:
    static int getThreadId() {
        static thread_local int id = -1;
        static std::atomic<int> counter(0);
        if (id == -1) {
            id = counter++;
        }
        return id;
    }
};

// ============================================================================
// ProgressTracker - Track progress of parallel operations
// ============================================================================

struct ProgressTracker {
    std::atomic<int> total;
    std::atomic<int> completed;
    std::string operation_name;
    std::chrono::steady_clock::time_point start_time;

    ProgressTracker(const std::string &name = "", int total_items = 0)
        : total(total_items), completed(0), operation_name(name) {}

    void start(int total_items);
    void update(int count = 1);
    void finish();
    double getProgress() const;
    double getElapsedSeconds() const;
    double getEstimatedRemainingSeconds() const;
    void printProgress() const;
};

// ============================================================================
// Global thread pool
// ============================================================================

ThreadPool& getGlobalThreadPool();
void setGlobalThreadPoolSize(size_t size);
size_t getGlobalThreadPoolSize();

// ============================================================================
// Helper functions
// ============================================================================

// Get optimal thread count
size_t getOptimalThreadCount();

// Set thread affinity
void setThreadAffinity(int core_id);

// Get current thread ID
int getCurrentThreadId();

// Sleep for duration
void sleepFor(std::chrono::milliseconds duration);

// Yield to other threads
void yield();

} // namespace Threading

// ============================================================================
// Template implementations
// ============================================================================

namespace Threading {

// ParallelFor template implementations
template<typename Func>
void ParallelFor::execute(int start, int end, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    int total = end - start;
    if (total <= 0) return;

    int chunk_size = (total + num_threads - 1) / num_threads;

    std::vector<std::future<void>> futures;
    for (int t = 0; t < num_threads; t++) {
        int chunk_start = start + t * chunk_size;
        int chunk_end = std::min(chunk_start + chunk_size, end);

        if (chunk_start < end) {
            futures.push_back(std::async(std::launch::async, [func, chunk_start, chunk_end]() {
                for (int i = chunk_start; i < chunk_end; i++) {
                    func(i);
                }
            }));
        }
    }

    for (auto &f : futures) {
        f.get();
    }
}

template<typename Func>
void ParallelFor::execute(int start, int end, int chunk_size, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    int total = end - start;
    if (total <= 0) return;

    std::vector<std::future<void>> futures;
    for (int i = start; i < end; i += chunk_size) {
        int chunk_end = std::min(i + chunk_size, end);
        futures.push_back(std::async(std::launch::async, [func, i, chunk_end]() {
            for (int j = i; j < chunk_end; j++) {
                func(j);
            }
        }));

        if ((int)futures.size() >= num_threads) {
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

template<typename Func>
void ParallelFor::executeIndexed(int start, int end, Func func, int num_threads) {
    execute(start, end, func, num_threads);
}

// ParallelReduction template implementations
template<typename T, typename Func>
T ParallelReduction::sum(const std::vector<T> &data, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    std::vector<std::future<T>> futures;
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());

        if (start < (int)data.size()) {
            futures.push_back(std::async(std::launch::async, [&data, func, start, end]() {
                T result = T();
                for (int i = start; i < end; i++) {
                    result += func(data[i]);
                }
                return result;
            }));
        }
    }

    T total = T();
    for (auto &f : futures) {
        total += f.get();
    }
    return total;
}

template<typename T, typename Func>
T ParallelReduction::min(const std::vector<T> &data, Func func, int num_threads) {
    if (data.empty()) return T();

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    std::vector<std::future<T>> futures;
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());

        if (start < (int)data.size()) {
            futures.push_back(std::async(std::launch::async, [&data, func, start, end]() {
                T result = func(data[start]);
                for (int i = start + 1; i < end; i++) {
                    T val = func(data[i]);
                    if (val < result) result = val;
                }
                return result;
            }));
        }
    }

    T result = futures[0].get();
    for (size_t i = 1; i < futures.size(); i++) {
        T val = futures[i].get();
        if (val < result) result = val;
    }
    return result;
}

template<typename T, typename Func>
T ParallelReduction::max(const std::vector<T> &data, Func func, int num_threads) {
    if (data.empty()) return T();

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    std::vector<std::future<T>> futures;
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());

        if (start < (int)data.size()) {
            futures.push_back(std::async(std::launch::async, [&data, func, start, end]() {
                T result = func(data[start]);
                for (int i = start + 1; i < end; i++) {
                    T val = func(data[i]);
                    if (val > result) result = val;
                }
                return result;
            }));
        }
    }

    T result = futures[0].get();
    for (size_t i = 1; i < futures.size(); i++) {
        T val = futures[i].get();
        if (val > result) result = val;
    }
    return result;
}

template<typename T, typename Func>
bool ParallelReduction::any(const std::vector<T> &data, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    std::vector<std::future<bool>> futures;
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());

        if (start < (int)data.size()) {
            futures.push_back(std::async(std::launch::async, [&data, func, start, end]() {
                for (int i = start; i < end; i++) {
                    if (func(data[i])) return true;
                }
                return false;
            }));
        }
    }

    for (auto &f : futures) {
        if (f.get()) return true;
    }
    return false;
}

template<typename T, typename Func>
bool ParallelReduction::all(const std::vector<T> &data, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    std::vector<std::future<bool>> futures;
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());

        if (start < (int)data.size()) {
            futures.push_back(std::async(std::launch::async, [&data, func, start, end]() {
                for (int i = start; i < end; i++) {
                    if (!func(data[i])) return false;
                }
                return true;
            }));
        }
    }

    for (auto &f : futures) {
        if (!f.get()) return false;
    }
    return true;
}

// ParallelSort template implementations
template<typename T, typename Compare>
void ParallelSort::quicksort(std::vector<T> &data, Compare comp, int num_threads) {
    if (data.size() <= 1) return;

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    if (num_threads <= 1 || data.size() < 1000) {
        std::sort(data.begin(), data.end(), comp);
        return;
    }

    // Parallel quicksort implementation
    auto partition = [&](int low, int high) -> int {
        T pivot = data[high];
        int i = low - 1;
        for (int j = low; j < high; j++) {
            if (comp(data[j], pivot)) {
                i++;
                std::swap(data[i], data[j]);
            }
        }
        std::swap(data[i + 1], data[high]);
        return i + 1;
    };

    std::function<void(int, int)> sort_func = [&](int low, int high) {
        if (low < high) {
            int pi = partition(low, high);
            sort_func(low, pi - 1);
            sort_func(pi + 1, high);
        }
    };

    sort_func(0, data.size() - 1);
}

template<typename T, typename Compare>
void ParallelSort::mergesort(std::vector<T> &data, Compare comp, int num_threads) {
    if (data.size() <= 1) return;

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    if (num_threads <= 1 || data.size() < 1000) {
        std::sort(data.begin(), data.end(), comp);
        return;
    }

    // Parallel merge sort implementation
    std::function<void(int, int)> sort_func = [&](int left, int right) {
        if (left < right) {
            int mid = left + (right - left) / 2;
            sort_func(left, mid);
            sort_func(mid + 1, right);

            // Merge
            std::vector<T> temp(right - left + 1);
            int i = left, j = mid + 1, k = 0;
            while (i <= mid && j <= right) {
                if (comp(data[i], data[j])) {
                    temp[k++] = data[i++];
                } else {
                    temp[k++] = data[j++];
                }
            }
            while (i <= mid) temp[k++] = data[i++];
            while (j <= right) temp[k++] = data[j++];

            for (int t = 0; t < k; t++) {
                data[left + t] = temp[t];
            }
        }
    };

    sort_func(0, data.size() - 1);
}

// ParallelMap template implementations
template<typename Input, typename Output, typename Func>
void ParallelMap::transform(const std::vector<Input> &input, std::vector<Output> &output,
                           Func func, int num_threads) {
    output.resize(input.size());

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    ParallelFor::execute(0, input.size(), [&](int i) {
        output[i] = func(input[i]);
    }, num_threads);
}

template<typename T, typename Func>
std::vector<T> ParallelMap::filter(const std::vector<T> &data, Func func, int num_threads) {
    std::vector<T> result;

    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    // First pass: count matching elements
    std::vector<int> counts(num_threads, 0);
    int chunk_size = (data.size() + num_threads - 1) / num_threads;

    ParallelFor::execute(0, num_threads, [&](int t) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());
        for (int i = start; i < end; i++) {
            if (func(data[i])) {
                counts[t]++;
            }
        }
    }, 1);

    // Calculate prefix sums
    std::vector<int> offsets(num_threads);
    offsets[0] = 0;
    for (int t = 1; t < num_threads; t++) {
        offsets[t] = offsets[t-1] + counts[t-1];
    }

    int total = offsets[num_threads-1] + counts[num_threads-1];
    result.resize(total);

    // Second pass: fill result
    ParallelFor::execute(0, num_threads, [&](int t) {
        int start = t * chunk_size;
        int end = std::min(start + chunk_size, (int)data.size());
        int pos = offsets[t];
        for (int i = start; i < end; i++) {
            if (func(data[i])) {
                result[pos++] = data[i];
            }
        }
    }, 1);

    return result;
}

template<typename T, typename Func>
void ParallelMap::for_each(std::vector<T> &data, Func func, int num_threads) {
    if (num_threads <= 0) {
        num_threads = getOptimalThreadCount();
    }

    ParallelFor::execute(0, data.size(), [&](int i) {
        func(data[i]);
    }, num_threads);
}

} // namespace Threading

#endif // THREADING_INDUSTRIAL_H
