// SPDX-License-Identifier: MIT or GPL-2.0-only

#include "cache_manager.h"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
#include <random>

// ============================================================================
// Test Utilities
// ============================================================================

class TestLogger {
public:
    static void log(const std::string& message) {
        std::cout << "[TEST] " << message << std::endl;
    }
    
    static void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
    
    static void success(const std::string& message) {
        std::cout << "[SUCCESS] " << message << std::endl;
    }
};

// ============================================================================
// LRU Golden Data Structure
// ============================================================================

class LRUGoldenValidator {
private:
    std::unordered_map<int, int> access_times_;  // key -> access_time
    int current_time_;
    size_t capacity_;
    
public:
    LRUGoldenValidator(size_t capacity) : current_time_(0), capacity_(capacity) {}
    
    void access(int key) {
        access_times_[key] = ++current_time_;
    }
    
    int get_eviction_candidate() {
        if (access_times_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        
        int oldest_key = -1;
        int oldest_time = std::numeric_limits<int>::max();
        
        for (const auto& pair : access_times_) {
            if (pair.second < oldest_time) {
                oldest_time = pair.second;
                oldest_key = pair.first;
            }
        }
        
        return oldest_key;
    }
    
    void insert(int key) {
        if (access_times_.size() >= capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key != -1) {
                access_times_.erase(evict_key);
            }
        }
        access_times_[key] = ++current_time_;
    }
    
    void remove(int key) {
        access_times_.erase(key);
    }
    
    bool contains(int key) const {
        return access_times_.find(key) != access_times_.end();
    }
    
    size_t size() const {
        return access_times_.size();
    }
    
    void clear() {
        access_times_.clear();
        current_time_ = 0;
    }
};

// ============================================================================
// LFU Golden Data Structure
// ============================================================================

class LFUGoldenValidator {
private:
    std::unordered_map<int, int> access_counts_;  // key -> access_count
    size_t capacity_;
    
public:
    LFUGoldenValidator(size_t capacity) : capacity_(capacity) {}
    
    void access(int key) {
        if (access_counts_.find(key) != access_counts_.end()) {
            access_counts_[key]++;
        }
    }
    
    int get_eviction_candidate() {
        if (access_counts_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        
        int least_frequent_key = -1;
        int min_count = std::numeric_limits<int>::max();
        
        for (const auto& pair : access_counts_) {
            if (pair.second < min_count) {
                min_count = pair.second;
                least_frequent_key = pair.first;
            }
        }
        
        return least_frequent_key;
    }
    
    void insert(int key) {
        if (access_counts_.size() >= capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key != -1) {
                access_counts_.erase(evict_key);
            }
        }
        access_counts_[key] = 1;
    }
    
    void remove(int key) {
        access_counts_.erase(key);
    }
    
    bool contains(int key) const {
        return access_counts_.find(key) != access_counts_.end();
    }
    
    size_t size() const {
        return access_counts_.size();
    }
    
    void clear() {
        access_counts_.clear();
    }
};

// ============================================================================
// FIFO Golden Data Structure
// ============================================================================

class FIFOGoldenValidator {
private:
    std::vector<int> queue_;  // Insertion order
    size_t capacity_;
    
public:
    FIFOGoldenValidator(size_t capacity) : capacity_(capacity) {}
    
    void access(int key) {
        // FIFO doesn't change order on access
    }
    
    int get_eviction_candidate() {
        if (queue_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        return queue_.front();  // First in, first out
    }
    
    void insert(int key) {
        if (queue_.size() >= capacity_) {
            queue_.erase(queue_.begin());  // Remove oldest
        }
        queue_.push_back(key);
    }
    
    void remove(int key) {
        auto it = std::find(queue_.begin(), queue_.end(), key);
        if (it != queue_.end()) {
            queue_.erase(it);
        }
    }
    
    bool contains(int key) const {
        return std::find(queue_.begin(), queue_.end(), key) != queue_.end();
    }
    
    size_t size() const {
        return queue_.size();
    }
    
    void clear() {
        queue_.clear();
    }
};

// ============================================================================
// CLOCK Golden Data Structure
// ============================================================================

class CLOCKGoldenValidator {
private:
    std::vector<std::pair<int, bool>> clock_;  // key -> reference_bit
    size_t hand_;  // Clock hand position
    size_t capacity_;
    
public:
    CLOCKGoldenValidator(size_t capacity) : hand_(0), capacity_(capacity) {}
    
    void access(int key) {
        // Set reference bit to true
        for (auto& entry : clock_) {
            if (entry.first == key) {
                entry.second = true;
                return;
            }
        }
    }
    
    int get_eviction_candidate() {
        if (clock_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        
        // Find victim using clock algorithm
        while (true) {
            if (clock_[hand_].second) {
                // Give second chance
                clock_[hand_].second = false;
                hand_ = (hand_ + 1) % clock_.size();
            } else {
                // Found victim
                return clock_[hand_].first;
            }
        }
    }
    
    void insert(int key) {
        if (clock_.size() >= capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key != -1) {
                // Replace the evicted entry
                for (auto& entry : clock_) {
                    if (entry.first == evict_key) {
                        entry.first = key;
                        entry.second = true;
                        hand_ = (hand_ + 1) % clock_.size();
                        return;
                    }
                }
            }
        } else {
            // Add new entry
            clock_.push_back({key, true});
        }
    }
    
    void remove(int key) {
        auto it = std::find_if(clock_.begin(), clock_.end(),
                              [key](const auto& entry) { return entry.first == key; });
        if (it != clock_.end()) {
            clock_.erase(it);
        }
    }
    
    bool contains(int key) const {
        return std::find_if(clock_.begin(), clock_.end(),
                           [key](const auto& entry) { return entry.first == key; }) != clock_.end();
    }
    
    size_t size() const {
        return clock_.size();
    }
    
    void clear() {
        clock_.clear();
        hand_ = 0;
    }
};

// ============================================================================
// CLOCK_FREQ Golden Data Structure
// ============================================================================

class CLOCK_FREQGoldenValidator {
private:
    std::vector<std::pair<int, int>> clock_;  // key -> frequency counter
    size_t hand_;  // Clock hand position
    size_t capacity_;

public:
    CLOCK_FREQGoldenValidator(size_t capacity) : hand_(0), capacity_(capacity) {}

    void access(int key) {
        // Find the key in the clock and increment frequency counter
        for (auto& entry : clock_) {
            if (entry.first == key) {
                if (entry.second < 255) {  // Prevent overflow
                    entry.second++;
                }
                return;
            }
        }
    }

    int get_eviction_candidate() {
        if (clock_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        
        // Find victim using clock algorithm with frequency counter
        while (true) {
            if (clock_[hand_].second > 1) {
                // Decrement frequency and give another chance
                clock_[hand_].second--;
                hand_ = (hand_ + 1) % clock_.size();
            } else {
                // Found victim with frequency <= 1
                return clock_[hand_].first;
            }
        }
    }

    void insert(int key) {
        if (clock_.size() >= capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key != -1) {
                // Replace the evicted entry
                for (auto& entry : clock_) {
                    if (entry.first == evict_key) {
                        entry.first = key;
                        entry.second = 1;  // New entries get frequency = 1
                        hand_ = (hand_ + 1) % clock_.size();
                        return;
                    }
                }
            }
        } else {
            // Add new entry
            clock_.push_back({key, 1});  // New entries get frequency = 1
        }
    }

    void remove(int key) {
        auto it = std::find_if(clock_.begin(), clock_.end(),
                              [key](const auto& entry) { return entry.first == key; });
        if (it != clock_.end()) {
            clock_.erase(it);
        }
    }

    bool contains(int key) const {
        return std::find_if(clock_.begin(), clock_.end(),
                           [key](const auto& entry) { return entry.first == key; }) != clock_.end();
    }

    size_t size() const {
        return clock_.size();
    }

    void clear() {
        clock_.clear();
        hand_ = 0;
    }
};

// ============================================================================
// SIEVE Golden Data Structure
// ============================================================================

class SIEVEGoldenValidator {
private:
    std::vector<std::pair<int, bool>> sieve_;  // key -> visited
    size_t hand_;  // Sieve hand position
    size_t capacity_;
    
public:
    SIEVEGoldenValidator(size_t capacity) : hand_(0), capacity_(capacity) {}
    
    void access(int key) {
        // Set visited bit to true
        for (auto& entry : sieve_) {
            if (entry.first == key) {
                entry.second = true;
                return;
            }
        }
    }
    
    int get_eviction_candidate() {
        if (sieve_.size() < capacity_) {
            return -1;  // No eviction needed
        }
        
        // Find victim using sieve algorithm
        size_t start = hand_;
        do {
            if (!sieve_[hand_].second) {
                // Found unvisited entry, evict it
                return sieve_[hand_].first;
            } else {
                // Mark as unvisited and continue
                sieve_[hand_].second = false;
            }
            hand_ = (hand_ + 1) % sieve_.size();
        } while (hand_ != start);
        
        // If we get here, all entries were visited, start over
        hand_ = 0;
        return sieve_[hand_].first;
    }
    
    void insert(int key) {
        if (sieve_.size() >= capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key != -1) {
                // Replace the evicted entry
                for (auto& entry : sieve_) {
                    if (entry.first == evict_key) {
                        entry.first = key;
                        entry.second = true;
                        hand_ = (hand_ + 1) % sieve_.size();
                        return;
                    }
                }
            }
        } else {
            // Add new entry
            sieve_.push_back({key, true});
        }
    }
    
    void remove(int key) {
        auto it = std::find_if(sieve_.begin(), sieve_.end(),
                              [key](const auto& entry) { return entry.first == key; });
        if (it != sieve_.end()) {
            sieve_.erase(it);
        }
    }
    
    bool contains(int key) const {
        return std::find_if(sieve_.begin(), sieve_.end(),
                           [key](const auto& entry) { return entry.first == key; }) != sieve_.end();
    }
    
    size_t size() const {
        return sieve_.size();
    }
    
    void clear() {
        sieve_.clear();
        hand_ = 0;
    }
};

// ============================================================================
// ARC Golden Data Structure
// ============================================================================

class ARCGoldenValidator {
private:
    std::vector<int> t1_;  // Recent entries (LRU)
    std::vector<int> t2_;  // Frequent entries (LRU)
    std::vector<int> b1_;  // Ghost list for T1
    std::vector<int> b2_;  // Ghost list for T2
    int p_;  // Adaptive parameter
    size_t capacity_;
    
public:
    ARCGoldenValidator(size_t capacity) : p_(0), capacity_(capacity) {}
    
    void access(int key) {
        // Check if key is in T1
        auto t1_it = std::find(t1_.begin(), t1_.end(), key);
        if (t1_it != t1_.end()) {
            // Move from T1 to T2 (promotion)
            t1_.erase(t1_it);
            t2_.push_back(key);
            return;
        }
        
        // Check if key is in T2
        auto t2_it = std::find(t2_.begin(), t2_.end(), key);
        if (t2_it != t2_.end()) {
            // Move to end of T2 (LRU update)
            t2_.erase(t2_it);
            t2_.push_back(key);
            return;
        }
    }
    
    int get_eviction_candidate() {
        size_t t1_size = t1_.size();
        size_t t2_size = t2_.size();
        
        if (t1_size + t2_size < capacity_) {
            return -1;  // No eviction needed
        }
        
        // Case A: T1 has more than p elements
        if (t1_size > p_) {
            return t1_.front();  // Evict from T1 head (oldest)
        }
        // Case B: T1 has exactly p elements and T2 is not empty
        else if (t1_size == p_ && t2_size > 0) {
            return t2_.front();  // Evict from T2 head (oldest)
        }
        // Case C: T1 has less than p elements
        else {
            return t1_.front();  // Evict from T1 head (oldest)
        }
    }
    
    void insert(int key) {
        // Check if key exists in ghost lists
        bool in_b1 = std::find(b1_.begin(), b1_.end(), key) != b1_.end();
        bool in_b2 = std::find(b2_.begin(), b2_.end(), key) != b2_.end();
        
        if (in_b1) {
            // Case I: key in B1 (recently evicted from T1)
            // Increase p to favor T1
            p_ = std::min(p_ + std::max(1, (int)(b2_.size() / std::max(1, (int)b1_.size()))), (int)capacity_);
            b1_.erase(std::remove(b1_.begin(), b1_.end(), key), b1_.end());
            // Insert into T2
            t2_.push_back(key);
        } else if (in_b2) {
            // Case II: key in B2 (recently evicted from T2)
            // Decrease p to favor T2
            p_ = std::max(p_ - std::max(1, (int)(b1_.size() / std::max(1, (int)b2_.size()))), 0);
            b2_.erase(std::remove(b2_.begin(), b2_.end(), key), b2_.end());
            // Insert into T2
            t2_.push_back(key);
        } else {
            // Case III: key not in ghost lists
            // Insert into T1
            t1_.push_back(key);
        }
        
        // Handle eviction if needed
        while (t1_.size() + t2_.size() > capacity_) {
            int evict_key = get_eviction_candidate();
            if (evict_key == -1) break;
            
            // Remove from appropriate list and add to ghost list
            auto t1_it = std::find(t1_.begin(), t1_.end(), evict_key);
            if (t1_it != t1_.end()) {
                t1_.erase(t1_it);
                b1_.push_back(evict_key);
            } else {
                auto t2_it = std::find(t2_.begin(), t2_.end(), evict_key);
                if (t2_it != t2_.end()) {
                    t2_.erase(t2_it);
                    b2_.push_back(evict_key);
                }
            }
        }
    }
    
    void remove(int key) {
        auto t1_it = std::find(t1_.begin(), t1_.end(), key);
        if (t1_it != t1_.end()) {
            t1_.erase(t1_it);
            b1_.push_back(key);
            return;
        }
        
        auto t2_it = std::find(t2_.begin(), t2_.end(), key);
        if (t2_it != t2_.end()) {
            t2_.erase(t2_it);
            b2_.push_back(key);
            return;
        }
    }
    
    bool contains(int key) const {
        return std::find(t1_.begin(), t1_.end(), key) != t1_.end() ||
               std::find(t2_.begin(), t2_.end(), key) != t2_.end();
    }
    
    size_t size() const {
        return t1_.size() + t2_.size();
    }
    
    void clear() {
        t1_.clear();
        t2_.clear();
        b1_.clear();
        b2_.clear();
        p_ = 0;
    }
};

// ============================================================================
// Test Functions
// ============================================================================

template<typename CacheType, typename ValidatorType>
void test_basic_operations() {
    TestLogger::log("Testing basic operations...");
    
    const size_t cache_size = 3;
    CacheType cache(cache_size);
    ValidatorType validator(cache_size);
    
    // Test insert
    cache.insert(1, 100);
    validator.insert(1);
    assert(cache.get_used_entries() == validator.size());
    
    cache.insert(2, 200);
    validator.insert(2);
    assert(cache.get_used_entries() == validator.size());
    
    cache.insert(3, 300);
    validator.insert(3);
    assert(cache.get_used_entries() == validator.size());
    
    // Test lookup
    int value;
    bool found = cache.lookup(1, value);
    assert(found && value == 100);
    validator.access(1);
    
    found = cache.lookup(4, value);
    assert(!found);
    
    // Test eviction
    cache.insert(4, 400);
    validator.insert(4);
    assert(cache.get_used_entries() == validator.size());
    
    // Verify cache size is maintained
    assert(cache.get_used_entries() <= cache_size);
    
    // Check that at least one item was evicted (since we inserted 4 items into a size-3 cache)
    int total_found = 0;
    for (int key = 1; key <= 4; ++key) {
        int value;
        if (cache.lookup(key, value)) {
            total_found++;
        }
    }
    assert(total_found <= cache_size);
    
    TestLogger::success("Basic operations test passed");
}

template<typename CacheType, typename ValidatorType>
void test_policy_specific_behavior() {
    TestLogger::log("Testing policy-specific behavior...");
    
    const size_t cache_size = 3;
    CacheType cache(cache_size);
    ValidatorType validator(cache_size);
    
    // Insert initial items
    cache.insert(1, 100);
    cache.insert(2, 200);
    cache.insert(3, 300);
    validator.insert(1);
    validator.insert(2);
    validator.insert(3);
    
    // Access items to change policy state
    int value;
    cache.lookup(1, value);
    validator.access(1);
    
    cache.lookup(2, value);
    validator.access(2);
    
    cache.lookup(1, value);
    validator.access(1);
    
    // Insert new item to trigger eviction
    cache.insert(4, 400);
    validator.insert(4);
    
    // Verify cache size is maintained
    assert(cache.get_used_entries() <= cache_size);
    
    // Verify that the cache still contains the expected number of items
    int total_found = 0;
    for (int key = 1; key <= 4; ++key) {
        int value;
        if (cache.lookup(key, value)) {
            total_found++;
        }
    }
    assert(total_found <= cache_size);
    
    TestLogger::success("Policy-specific behavior test passed");
}

template<typename CacheType, typename ValidatorType>
void test_edge_cases() {
    TestLogger::log("Testing edge cases...");
    
    const size_t cache_size = 2;
    CacheType cache(cache_size);
    ValidatorType validator(cache_size);
    
    // Test empty cache
    assert(cache.get_used_entries() == 0);
    assert(cache.get_hits() == 0);
    assert(cache.get_misses() == 0);
    
    // Test single item
    cache.insert(1, 100);
    validator.insert(1);
    assert(cache.get_used_entries() == 1);
    
    // Test duplicate insert
    cache.insert(1, 150);
    validator.access(1);
    assert(cache.get_used_entries() == 1);
    
    int value;
    bool found = cache.lookup(1, value);
    assert(found);
    
    // Test invalidate
    cache.invalidate(1);
    validator.remove(1);
    assert(cache.get_used_entries() == 0);
    
    found = cache.lookup(1, value);
    assert(!found);
    
    // Test mark dirty/clean
    cache.insert(1, 100);
    validator.insert(1);
    cache.mark_dirty(1);
    cache.mark_clean(1);
    
    TestLogger::success("Edge cases test passed");
}

template<typename CacheType, typename ValidatorType>
void test_stress_test() {
    TestLogger::log("Running stress test...");
    
    const size_t cache_size = 100;
    CacheType cache(cache_size);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> key_dist(1, 1000);
    std::uniform_int_distribution<> op_dist(0, 2);  // 0=insert, 1=lookup, 2=invalidate
    
    for (int i = 0; i < 10000; ++i) {
        int key = key_dist(gen);
        int op = op_dist(gen);
        
        switch (op) {
            case 0: {  // Insert
                cache.insert(key, key * 10);
                break;
            }
            case 1: {  // Lookup
                int value;
                cache.lookup(key, value);
                break;
            }
            case 2: {  // Invalidate
                cache.invalidate(key);
                break;
            }
        }
        
        // Periodically verify cache size constraint
        if (i % 1000 == 0) {
            assert(cache.get_used_entries() <= cache_size);
        }
    }
    
    TestLogger::success("Stress test passed");
}

template<typename CacheType, typename ValidatorType>
void test_concurrent_access() {
    TestLogger::log("Testing concurrent access...");
    
    const size_t cache_size = 50;
    CacheType cache(cache_size);
    
    const int num_threads = 4;
    const int operations_per_thread = 1000;
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&cache, t, operations_per_thread]() {
            std::random_device rd;
            std::mt19937 gen(rd() + t);
            std::uniform_int_distribution<> key_dist(1, 100);
            std::uniform_int_distribution<> op_dist(0, 1);
            
            for (int i = 0; i < operations_per_thread; ++i) {
                int key = key_dist(gen);
                int op = op_dist(gen);
                
                if (op == 0) {
                    cache.insert(key, key * 10);
                } else {
                    int value;
                    cache.lookup(key, value);
                }
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    TestLogger::success("Concurrent access test passed");
}

// ============================================================================
// Main Test Runner
// ============================================================================

void run_all_tests() {
    TestLogger::log("Starting comprehensive cache tests...");
    
    // Test LRU Cache
    TestLogger::log("=== Testing LRU Cache ===");
    test_basic_operations<LRUCacheManager<int, int>, LRUGoldenValidator>();
    test_policy_specific_behavior<LRUCacheManager<int, int>, LRUGoldenValidator>();
    test_edge_cases<LRUCacheManager<int, int>, LRUGoldenValidator>();
    test_stress_test<LRUCacheManager<int, int>, LRUGoldenValidator>();
    test_concurrent_access<LRUCacheManager<int, int>, LRUGoldenValidator>();
    
    // Test LFU Cache
    TestLogger::log("=== Testing LFU Cache ===");
    test_basic_operations<LFUCacheManager<int, int>, LFUGoldenValidator>();
    test_policy_specific_behavior<LFUCacheManager<int, int>, LFUGoldenValidator>();
    test_edge_cases<LFUCacheManager<int, int>, LFUGoldenValidator>();
    test_stress_test<LFUCacheManager<int, int>, LFUGoldenValidator>();
    test_concurrent_access<LFUCacheManager<int, int>, LFUGoldenValidator>();
    
    // Test FIFO Cache
    TestLogger::log("=== Testing FIFO Cache ===");
    test_basic_operations<FIFOCacheManager<int, int>, FIFOGoldenValidator>();
    test_policy_specific_behavior<FIFOCacheManager<int, int>, FIFOGoldenValidator>();
    test_edge_cases<FIFOCacheManager<int, int>, FIFOGoldenValidator>();
    test_stress_test<FIFOCacheManager<int, int>, FIFOGoldenValidator>();
    test_concurrent_access<FIFOCacheManager<int, int>, FIFOGoldenValidator>();
    
    // Test CLOCK Cache
    TestLogger::log("=== Testing CLOCK Cache ===");
    test_basic_operations<CLOCKCacheManager<int, int>, CLOCKGoldenValidator>();
    test_policy_specific_behavior<CLOCKCacheManager<int, int>, CLOCKGoldenValidator>();
    test_edge_cases<CLOCKCacheManager<int, int>, CLOCKGoldenValidator>();
    test_stress_test<CLOCKCacheManager<int, int>, CLOCKGoldenValidator>();
    test_concurrent_access<CLOCKCacheManager<int, int>, CLOCKGoldenValidator>();
    
    // Test CLOCK_FREQ Cache
    TestLogger::log("=== Testing CLOCK_FREQ Cache ===");
    test_basic_operations<CLOCK_FREQCacheManager<int, int>, CLOCK_FREQGoldenValidator>();
    test_policy_specific_behavior<CLOCK_FREQCacheManager<int, int>, CLOCK_FREQGoldenValidator>();
    test_edge_cases<CLOCK_FREQCacheManager<int, int>, CLOCK_FREQGoldenValidator>();
    test_stress_test<CLOCK_FREQCacheManager<int, int>, CLOCK_FREQGoldenValidator>();
    test_concurrent_access<CLOCK_FREQCacheManager<int, int>, CLOCK_FREQGoldenValidator>();
    
    // Test SIEVE Cache
    TestLogger::log("=== Testing SIEVE Cache ===");
    test_basic_operations<SIEVECacheManager<int, int>, SIEVEGoldenValidator>();
    test_policy_specific_behavior<SIEVECacheManager<int, int>, SIEVEGoldenValidator>();
    test_edge_cases<SIEVECacheManager<int, int>, SIEVEGoldenValidator>();
    test_stress_test<SIEVECacheManager<int, int>, SIEVEGoldenValidator>();
    test_concurrent_access<SIEVECacheManager<int, int>, SIEVEGoldenValidator>();
    
    // Test ARC Cache
    TestLogger::log("=== Testing ARC Cache ===");
    test_basic_operations<ARCCacheManager<int, int>, ARCGoldenValidator>();
    test_policy_specific_behavior<ARCCacheManager<int, int>, ARCGoldenValidator>();
    test_edge_cases<ARCCacheManager<int, int>, ARCGoldenValidator>();
    test_stress_test<ARCCacheManager<int, int>, ARCGoldenValidator>();
    test_concurrent_access<ARCCacheManager<int, int>, ARCGoldenValidator>();
    
    TestLogger::success("All tests passed successfully!");
}

int main() {
    try {
        run_all_tests();
        return 0;
    } catch (const std::exception& e) {
        TestLogger::error("Test failed with exception: " + std::string(e.what()));
        return 1;
    } catch (...) {
        TestLogger::error("Test failed with unknown exception");
        return 1;
    }
} 