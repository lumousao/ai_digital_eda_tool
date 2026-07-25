/**
 * Performance Optimization - Industrial-grade performance optimization
 *
 * Features:
 * - Incremental algorithms
 * - Caching mechanisms
 * - Memory pooling
 * - Profile-guided optimization
 * - Lazy evaluation
 * - Memoization
 * - Hash-based optimization
 * - Cache-aware algorithms
 */

#ifndef PERFORMANCE_INDUSTRIAL_H
#define PERFORMANCE_INDUSTRIAL_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>
#include <algorithm>
#include <future>
#include <thread>
#include <mutex>
#include <list>
#include <iostream>
#include <ostream>

namespace Performance {

// ============================================================================
// Forward declarations
// ============================================================================

struct MemoryPool;
struct Profiler;
struct IncrementalEngine;
struct HashCache;

// ============================================================================
// Cache - Generic cache implementation
// ============================================================================

template<typename Key, typename Value>
struct Cache {
    std::unordered_map<Key, Value> data;
    std::unordered_map<Key, std::chrono::steady_clock::time_point> timestamps;
    size_t max_size;
    std::chrono::milliseconds ttl;
    mutable std::mutex mutex;

    Cache(size_t max = 1000, std::chrono::milliseconds ttl_ms = std::chrono::milliseconds(60000))
        : max_size(max), ttl(ttl_ms) {}

    void put(const Key &key, const Value &value) {
        std::lock_guard<std::mutex> lock(mutex);
        data[key] = value;
        timestamps[key] = std::chrono::steady_clock::now();

        // Evict if over capacity
        if (data.size() > max_size) {
            evictOldest();
        }
    }

    bool get(const Key &key, Value &value) const {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = data.find(key);
        if (it == data.end()) {
            return false;
        }

        // Check TTL
        auto ts = timestamps.at(key);
        if (std::chrono::steady_clock::now() - ts > ttl) {
            return false;
        }

        value = it->second;
        return true;
    }

    bool has(const Key &key) const {
        std::lock_guard<std::mutex> lock(mutex);
        return data.find(key) != data.end();
    }

    void invalidate(const Key &key) {
        std::lock_guard<std::mutex> lock(mutex);
        data.erase(key);
        timestamps.erase(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        data.clear();
        timestamps.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return data.size();
    }

private:
    void evictOldest() {
        if (data.empty()) return;

        auto oldest = timestamps.begin();
        for (auto it = timestamps.begin(); it != timestamps.end(); ++it) {
            if (it->second < oldest->second) {
                oldest = it;
            }
        }

        data.erase(oldest->first);
        timestamps.erase(oldest);
    }
};

// ============================================================================
// LRU Cache - Least Recently Used cache
// ============================================================================

template<typename Key, typename Value>
struct LRUCache {
    std::list<std::pair<Key, Value>> items;
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator> index;
    size_t max_size;

    LRUCache(size_t max = 1000) : max_size(max) {}

    void put(const Key &key, const Value &value) {
        auto it = index.find(key);
        if (it != index.end()) {
            items.erase(it->second);
        }

        items.push_front({key, value});
        index[key] = items.begin();

        if (items.size() > max_size) {
            auto last = items.end();
            --last;
            index.erase(last->first);
            items.erase(last);
        }
    }

    bool get(const Key &key, Value &value) {
        auto it = index.find(key);
        if (it == index.end()) {
            return false;
        }

        // Move to front
        items.splice(items.begin(), items, it->second);
        value = it->second->second;
        return true;
    }

    bool has(const Key &key) const {
        return index.find(key) != index.end();
    }

    void invalidate(const Key &key) {
        auto it = index.find(key);
        if (it != index.end()) {
            items.erase(it->second);
            index.erase(it);
        }
    }

    void clear() {
        items.clear();
        index.clear();
    }

    size_t size() const {
        return items.size();
    }
};

// ============================================================================
// MemoryPool - Memory pool for fast allocation
// ============================================================================

struct MemoryPool {
    struct Block {
        char *data;
        size_t size;
        bool in_use;
    };

    std::vector<Block> blocks;
    size_t block_size;
    size_t total_allocated;
    size_t total_in_use;

    MemoryPool(size_t block_size = 4096)
        : block_size(block_size), total_allocated(0), total_in_use(0) {}

    ~MemoryPool() {
        for (auto &block : blocks) {
            delete[] block.data;
        }
    }

    void *allocate(size_t size) {
        // Find free block
        for (auto &block : blocks) {
            if (!block.in_use && block.size >= size) {
                block.in_use = true;
                total_in_use += block.size;
                return block.data;
            }
        }

        // Allocate new block
        Block new_block;
        new_block.size = std::max(size, block_size);
        new_block.data = new char[new_block.size];
        new_block.in_use = true;
        blocks.push_back(new_block);

        total_allocated += new_block.size;
        total_in_use += new_block.size;

        return new_block.data;
    }

    void deallocate(void *ptr) {
        for (auto &block : blocks) {
            if (block.data == ptr) {
                block.in_use = false;
                total_in_use -= block.size;
                return;
            }
        }
    }

    size_t getFragmentation() const {
        if (total_allocated == 0) return 0;
        return (total_allocated - total_in_use) * 100 / total_allocated;
    }

    void compact() {
        // Remove unused blocks
        blocks.erase(
            std::remove_if(blocks.begin(), blocks.end(),
                [](const Block &b) { return !b.in_use; }),
            blocks.end()
        );
    }
};

// ============================================================================
// Profiler - Performance profiler
// ============================================================================

struct Profiler {
    struct Timer {
        std::string name;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point end;
        bool running;

        Timer() : running(false) {}
        Timer(const std::string &n) : name(n), running(false) {}
    };

    struct Stats {
        std::string name;
        int call_count;
        double total_time_ms;
        double min_time_ms;
        double max_time_ms;
        double avg_time_ms;

        Stats() : call_count(0), total_time_ms(0.0),
                  min_time_ms(1e30), max_time_ms(0.0), avg_time_ms(0.0) {}
    };

    std::map<std::string, Stats> stats;
    std::map<std::string, Timer> timers;
    bool enabled;

    Profiler() : enabled(true) {}

    void start(const std::string &name) {
        if (!enabled) return;
        timers[name] = Timer(name);
        timers[name].start = std::chrono::steady_clock::now();
        timers[name].running = true;
    }

    void stop(const std::string &name) {
        if (!enabled) return;
        auto it = timers.find(name);
        if (it != timers.end() && it->second.running) {
            it->second.end = std::chrono::steady_clock::now();
            it->second.running = false;

            double duration = std::chrono::duration<double, std::milli>(
                it->second.end - it->second.start).count();

            auto &s = stats[name];
            s.name = name;
            s.call_count++;
            s.total_time_ms += duration;
            s.min_time_ms = std::min(s.min_time_ms, duration);
            s.max_time_ms = std::max(s.max_time_ms, duration);
            s.avg_time_ms = s.total_time_ms / s.call_count;
        }
    }

    Stats getStats(const std::string &name) const {
        auto it = stats.find(name);
        if (it != stats.end()) {
            return it->second;
        }
        return Stats();
    }

    void printStats() const {
        std::cout << "Performance Statistics:" << std::endl;
        std::cout << "========================" << std::endl;

        for (const auto &pair : stats) {
            const auto &s = pair.second;
            std::cout << s.name << ":" << std::endl;
            std::cout << "  Calls: " << s.call_count << std::endl;
            std::cout << "  Total: " << s.total_time_ms << " ms" << std::endl;
            std::cout << "  Avg: " << s.avg_time_ms << " ms" << std::endl;
            std::cout << "  Min: " << s.min_time_ms << " ms" << std::endl;
            std::cout << "  Max: " << s.max_time_ms << " ms" << std::endl;
            std::cout << std::endl;
        }
    }

    void reset() {
        stats.clear();
        timers.clear();
    }
};

// ============================================================================
// IncrementalEngine - Incremental computation engine
// ============================================================================

struct IncrementalEngine {
    struct Dependency {
        std::string name;
        std::function<bool()> is_valid;
        std::function<void()> invalidate;
    };

    std::map<std::string, Dependency> dependencies;
    std::map<std::string, std::function<void()>> computations;
    std::set<std::string> dirty;
    Profiler *profiler;

    IncrementalEngine() : profiler(nullptr) {}

    void setProfiler(Profiler *p) { profiler = p; }

    void registerDependency(const std::string &name,
                           std::function<bool()> is_valid,
                           std::function<void()> invalidate) {
        dependencies[name] = {name, is_valid, invalidate};
    }

    void registerComputation(const std::string &name,
                            std::function<void()> compute) {
        computations[name] = compute;
    }

    void markDirty(const std::string &name) {
        dirty.insert(name);
    }

    void update() {
        if (profiler) profiler->start("incremental_update");

        for (const auto &name : dirty) {
            auto it = computations.find(name);
            if (it != computations.end()) {
                it->second();
            }
        }

        dirty.clear();

        if (profiler) profiler->stop("incremental_update");
    }

    void updateIfDirty() {
        if (!dirty.empty()) {
            update();
        }
    }
};

// ============================================================================
// HashCache - Hash-based caching
// ============================================================================

struct HashCache {
    struct CacheEntry {
        std::string key;
        std::string value;
        size_t hash;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::vector<CacheEntry> entries;
    std::unordered_map<std::string, size_t> index;
    size_t max_size;

    HashCache(size_t max = 10000) : max_size(max) {}

    void put(const std::string &key, const std::string &value) {
        size_t hash = std::hash<std::string>{}(value);

        auto it = index.find(key);
        if (it != index.end()) {
            entries[it->second].value = value;
            entries[it->second].hash = hash;
            entries[it->second].timestamp = std::chrono::steady_clock::now();
            return;
        }

        if (entries.size() >= max_size) {
            // Evict oldest
            size_t oldest = 0;
            for (size_t i = 1; i < entries.size(); i++) {
                if (entries[i].timestamp < entries[oldest].timestamp) {
                    oldest = i;
                }
            }
            index.erase(entries[oldest].key);
            entries[oldest] = {key, value, hash, std::chrono::steady_clock::now()};
            index[key] = oldest;
        } else {
            index[key] = entries.size();
            entries.push_back({key, value, hash, std::chrono::steady_clock::now()});
        }
    }

    bool get(const std::string &key, std::string &value) const {
        auto it = index.find(key);
        if (it == index.end()) {
            return false;
        }
        value = entries[it->second].value;
        return true;
    }

    bool has(const std::string &key) const {
        return index.find(key) != index.end();
    }

    size_t getHash(const std::string &key) const {
        auto it = index.find(key);
        if (it == index.end()) {
            return 0;
        }
        return entries[it->second].hash;
    }

    void invalidate(const std::string &key) {
        auto it = index.find(key);
        if (it != index.end()) {
            entries[it->second] = entries.back();
            index[entries.back().key] = it->second;
            entries.pop_back();
            index.erase(it);
        }
    }

    void clear() {
        entries.clear();
        index.clear();
    }

    size_t size() const {
        return entries.size();
    }
};

// ============================================================================
// Memoization - Function memoization
// ============================================================================

template<typename Result, typename... Args>
struct Memoizer {
    std::function<Result(Args...)> func;
    std::map<std::tuple<Args...>, Result> cache;
    Profiler *profiler;

    Memoizer(std::function<Result(Args...)> f) : func(f), profiler(nullptr) {}

    void setProfiler(Profiler *p) { profiler = p; }

    Result operator()(Args... args) {
        if (profiler) profiler->start("memoize");

        auto key = std::make_tuple(args...);
        auto it = cache.find(key);
        if (it != cache.end()) {
            if (profiler) profiler->stop("memoize");
            return it->second;
        }

        Result result = func(args...);
        cache[key] = result;

        if (profiler) profiler->stop("memoize");
        return result;
    }

    void clear() {
        cache.clear();
    }
};

// ============================================================================
// BatchProcessor - Batch processing for better cache utilization
// ============================================================================

template<typename Input, typename Output>
struct BatchProcessor {
    std::function<Output(const Input&)> process_func;
    size_t batch_size;
    Profiler *profiler;

    BatchProcessor(std::function<Output(const Input&)> func, size_t batch = 1000)
        : process_func(func), batch_size(batch), profiler(nullptr) {}

    void setProfiler(Profiler *p) { profiler = p; }

    std::vector<Output> process(const std::vector<Input> &inputs) {
        if (profiler) profiler->start("batch_process");

        std::vector<Output> results;
        results.reserve(inputs.size());

        for (size_t i = 0; i < inputs.size(); i += batch_size) {
            size_t end = std::min(i + batch_size, inputs.size());
            for (size_t j = i; j < end; j++) {
                results.push_back(process_func(inputs[j]));
            }
        }

        if (profiler) profiler->stop("batch_process");
        return results;
    }
};

// ============================================================================
// ParallelBatchProcessor - Parallel batch processing
// ============================================================================

template<typename Input, typename Output>
struct ParallelBatchProcessor {
    std::function<Output(const Input&)> process_func;
    size_t batch_size;
    int num_threads;
    Profiler *profiler;

    ParallelBatchProcessor(std::function<Output(const Input&)> func,
                          size_t batch = 1000, int threads = 0)
        : process_func(func), batch_size(batch), num_threads(threads), profiler(nullptr) {}

    void setProfiler(Profiler *p) { profiler = p; }

    std::vector<Output> process(const std::vector<Input> &inputs) {
        if (profiler) profiler->start("parallel_batch_process");

        std::vector<Output> results(inputs.size());

        if (num_threads <= 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }

        std::vector<std::future<void>> futures;
        for (int t = 0; t < num_threads; t++) {
            size_t start = t * batch_size;
            size_t end = std::min(start + batch_size, inputs.size());

            if (start < inputs.size()) {
                futures.push_back(std::async(std::launch::async,
                    [this, &inputs, &results, start, end]() {
                        for (size_t i = start; i < end; i++) {
                            results[i] = process_func(inputs[i]);
                        }
                    }));
            }
        }

        for (auto &f : futures) {
            f.get();
        }

        if (profiler) profiler->stop("parallel_batch_process");
        return results;
    }
};

// ============================================================================
// Global performance utilities
// ============================================================================

// Get global profiler
Profiler& getGlobalProfiler();

// Get global memory pool
MemoryPool& getGlobalMemoryPool();

// Time a function
template<typename Func>
double timeFunction(Func func) {
    auto start = std::chrono::steady_clock::now();
    func();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// Profile a function
template<typename Func>
double profileFunction(const std::string &name, Func func) {
    auto &profiler = getGlobalProfiler();
    profiler.start(name);
    func();
    profiler.stop(name);
    return profiler.getStats(name).avg_time_ms;
}

// Memory usage tracking
size_t getMemoryUsage();
size_t getPeakMemoryUsage();

} // namespace Performance

#endif // PERFORMANCE_INDUSTRIAL_H
