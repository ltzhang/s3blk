// SPDX-License-Identifier: MIT or GPL-2.0-only

#include "cache_manager.h"
#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

// ============================================================================
// Access Pattern Generators
// ============================================================================

enum class AccessPattern {
    UNIFORM,
    ZIPF,
    GAUSSIAN,
    SEQUENTIAL,
    SEQUENTIAL_WITH_JUMP,
    EXPONENTIAL
};

class AccessPatternGenerator {
private:
    std::mt19937 gen_;
    std::uniform_int_distribution<uint64_t> uniform_dist_;
    std::normal_distribution<double> normal_dist_;
    std::exponential_distribution<double> exp_dist_;
    
    uint64_t key_space_size_;
    uint64_t current_seq_;
    uint64_t jump_size_;
    
public:
    AccessPatternGenerator(uint64_t key_space_size, uint64_t seed = 42)
        : gen_(seed), uniform_dist_(0, key_space_size - 1),
          normal_dist_(key_space_size / 2.0, key_space_size / 8.0),
          exp_dist_(1.0 / (key_space_size / 10.0)),
          key_space_size_(key_space_size), current_seq_(0), jump_size_(key_space_size / 10) {}
    
    uint64_t generate_key(AccessPattern pattern) {
        switch (pattern) {
            case AccessPattern::UNIFORM:
                return uniform_dist_(gen_);
                
            case AccessPattern::ZIPF: {
                // Generate Zipf distribution using rejection sampling
                double u = std::uniform_real_distribution<>(0.0, 1.0)(gen_);
                double v = std::uniform_real_distribution<>(0.0, 1.0)(gen_);
                double x = std::floor(std::pow(u, -1.0));
                double t = std::pow(1.0 + 1.0/x, 2.0);
                if (v * x * (t - 1.0) / (t - 0.5) < 1.0) {
                    return static_cast<uint64_t>(x - 1) % key_space_size_;
                }
                return generate_key(AccessPattern::ZIPF); // Retry
            }
                
            case AccessPattern::GAUSSIAN: {
                double value = normal_dist_(gen_);
                // Clamp to valid range
                value = std::max(0.0, std::min(static_cast<double>(key_space_size_ - 1), value));
                return static_cast<uint64_t>(value);
            }
                
            case AccessPattern::SEQUENTIAL: {
                uint64_t key = current_seq_ % key_space_size_;
                current_seq_++;
                return key;
            }
                
            case AccessPattern::SEQUENTIAL_WITH_JUMP: {
                uint64_t key = current_seq_ % key_space_size_;
                current_seq_++;
                // Occasionally jump by a random amount between 0 and jump_size_
                if (current_seq_ % 1000 == 0) {
                    uint64_t jump_amount = uniform_dist_(gen_) % (jump_size_ + 1);
                    current_seq_ = (current_seq_ + jump_amount) % key_space_size_;
                }
                return key;
            }
                
            case AccessPattern::EXPONENTIAL: {
                double value = exp_dist_(gen_);
                // Clamp to valid range
                value = std::min(static_cast<double>(key_space_size_ - 1), value);
                return static_cast<uint64_t>(value);
            }
        }
        return 0;
    }
    
    void reset() {
        current_seq_ = 0;
    }
};

// ============================================================================
// Performance Test Framework
// ============================================================================

struct TestResult {
    std::string policy_name;
    std::string pattern_name;
    uint64_t cache_size;
    uint64_t key_space_size;
    uint64_t total_operations;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    double hit_ratio;
    double miss_ratio;
    double avg_operation_time_ns;
    double throughput_ops_per_sec;
};

class CachePerformanceTester {
private:
    std::vector<TestResult> results_;
    
    template<typename CacheType>
    TestResult run_single_test(const std::string& policy_name, 
                              const std::string& pattern_name,
                              uint64_t cache_size,
                              uint64_t key_space_size,
                              uint64_t num_operations,
                              AccessPattern pattern,
                              uint64_t report_interval = 10000) {
        
        CacheType cache(cache_size);
        AccessPatternGenerator generator(key_space_size);
        
        TestResult result;
        result.policy_name = policy_name;
        result.pattern_name = pattern_name;
        result.cache_size = cache_size;
        result.key_space_size = key_space_size;
        result.total_operations = num_operations;
        
        // Warm up the cache with some initial inserts
        uint64_t warmup_ops = std::min(cache_size / 2, num_operations / 10);
        for (uint64_t i = 0; i < warmup_ops; ++i) {
            uint64_t key = generator.generate_key(pattern);
            cache.insert(key, key * 2); // Simple value
        }
        
        // Reset generator for actual test
        generator.reset();
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        uint64_t hits_before = cache.get_hits();
        uint64_t misses_before = cache.get_misses();
        uint64_t evictions_before = cache.get_evictions();
        
        // Statistics reporting interval
        uint64_t last_report_ops = 0;
        
        for (uint64_t i = 0; i < num_operations; ++i) {
            uint64_t key = generator.generate_key(pattern);
            uint64_t value;
            
            if (!cache.lookup(key, value)) {
                // Miss - insert the key
                cache.insert(key, key * 2);
            }
            
            // Print statistics periodically
            if ((i + 1) % report_interval == 0) {
                uint64_t current_hits = cache.get_hits() - hits_before;
                uint64_t current_misses = cache.get_misses() - misses_before;
                uint64_t current_evictions = cache.get_evictions() - evictions_before;
                uint64_t ops_since_last_report = i + 1 - last_report_ops;
                
                double current_hit_ratio = static_cast<double>(current_hits) / (i + 1);
                double current_miss_ratio = static_cast<double>(current_misses) / (i + 1);
                
                auto current_time = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
                double ops_per_sec = (i + 1) * 1000.0 / elapsed.count();
                
                std::cout << "  [" << policy_name << "/" << pattern_name << "] "
                          << "Ops: " << std::setw(8) << (i + 1) << "/" << num_operations
                          << " (" << std::fixed << std::setprecision(1) << ((i + 1) * 100.0 / num_operations) << "%)"
                          << " | Hit: " << std::fixed << std::setprecision(2) << (current_hit_ratio * 100) << "%"
                          << " | Miss: " << std::fixed << std::setprecision(2) << (current_miss_ratio * 100) << "%"
                          << " | Evict: " << std::setw(6) << current_evictions
                          << " | Throughput: " << std::fixed << std::setprecision(0) << ops_per_sec << " ops/sec"
                          << std::endl;
                
                last_report_ops = i + 1;
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        
        result.hits = cache.get_hits() - hits_before;
        result.misses = cache.get_misses() - misses_before;
        result.evictions = cache.get_evictions() - evictions_before;
        result.hit_ratio = static_cast<double>(result.hits) / num_operations;
        result.miss_ratio = static_cast<double>(result.misses) / num_operations;
        
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        result.avg_operation_time_ns = static_cast<double>(duration.count()) / num_operations;
        result.throughput_ops_per_sec = 1e9 / result.avg_operation_time_ns;
        
        return result;
    }
    
    template<typename CacheType>
    TestResult run_multi_threaded_test(const std::string& policy_name, 
                                      const std::string& pattern_name,
                                      uint64_t cache_size,
                                      uint64_t key_space_size,
                                      uint64_t num_operations,
                                      AccessPattern pattern,
                                      uint64_t num_threads,
                                      uint64_t report_interval = 10000) {
        
        CacheType cache(cache_size);
        std::atomic<uint64_t> completed_operations{0};
        std::mutex print_mutex;
        
        TestResult result;
        result.policy_name = policy_name;
        result.pattern_name = pattern_name;
        result.cache_size = cache_size;
        result.key_space_size = key_space_size;
        result.total_operations = num_operations * num_threads;
        
        // Warm up the cache with some initial inserts
        uint64_t warmup_ops = std::min(cache_size / 2, (num_operations * num_threads) / 10);
        AccessPatternGenerator warmup_generator(key_space_size);
        for (uint64_t i = 0; i < warmup_ops; ++i) {
            uint64_t key = warmup_generator.generate_key(pattern);
            cache.insert(key, key * 2);
        }
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Thread worker function
        auto worker = [&](uint64_t thread_id, uint64_t thread_ops) {
            AccessPatternGenerator generator(key_space_size, 42 + thread_id); // Different seed per thread
            
            for (uint64_t i = 0; i < thread_ops; ++i) {
                uint64_t key = generator.generate_key(pattern);
                uint64_t value;
                
                if (!cache.lookup(key, value)) {
                    // Miss - insert the key
                    cache.insert(key, key * 2);
                }
                
                // Update global counters
                uint64_t current_ops = completed_operations.fetch_add(1) + 1;
                
                // Print statistics periodically (only from one thread to avoid spam)
                if (thread_id == 0 && current_ops % report_interval == 0) {
                    std::lock_guard<std::mutex> lock(print_mutex);
                    
                    uint64_t current_hits = cache.get_hits();
                    uint64_t current_misses = cache.get_misses();
                    uint64_t current_evictions = cache.get_evictions();
                    
                    double current_hit_ratio = static_cast<double>(current_hits) / current_ops;
                    double current_miss_ratio = static_cast<double>(current_misses) / current_ops;
                    
                    auto current_time = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
                    double ops_per_sec = current_ops * 1000.0 / elapsed.count();
                    
                    std::cout << "  [" << policy_name << "/" << pattern_name << "] "
                              << "Threads: " << num_threads << " | "
                              << "Ops: " << std::setw(8) << current_ops << "/" << (num_operations * num_threads)
                              << " (" << std::fixed << std::setprecision(1) << (current_ops * 100.0 / (num_operations * num_threads)) << "%)"
                              << " | Hit: " << std::fixed << std::setprecision(2) << (current_hit_ratio * 100) << "%"
                              << " | Miss: " << std::fixed << std::setprecision(2) << (current_miss_ratio * 100) << "%"
                              << " | Evict: " << std::setw(6) << current_evictions
                              << " | Throughput: " << std::fixed << std::setprecision(0) << ops_per_sec << " ops/sec"
                              << std::endl;
                }
            }
        };
        
        // Create and start threads
        std::vector<std::thread> threads;
        for (uint64_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker, i, num_operations);
        }
        
        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        
        // Get final statistics directly from the cache
        result.hits = cache.get_hits();
        result.misses = cache.get_misses();
        result.evictions = cache.get_evictions();
        result.hit_ratio = static_cast<double>(result.hits) / (num_operations * num_threads);
        result.miss_ratio = static_cast<double>(result.misses) / (num_operations * num_threads);
        
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        result.avg_operation_time_ns = static_cast<double>(duration.count()) / (num_operations * num_threads);
        result.throughput_ops_per_sec = 1e9 / result.avg_operation_time_ns;
        
        return result;
    }
    
public:
    void run_comprehensive_test(uint64_t cache_size = 1000000,
                               uint64_t key_space_size = 10000000,
                               uint64_t num_operations = 5000000,
                               uint64_t report_interval = 10000,
                               uint64_t num_threads = 1) {
        
        std::cout << "=== Cache Performance Test ===" << std::endl;
        std::cout << "Cache Size: " << cache_size << std::endl;
        std::cout << "Key Space Size: " << key_space_size << std::endl;
        std::cout << "Operations per thread: " << num_operations << std::endl;
        std::cout << "Number of threads: " << num_threads << std::endl;
        std::cout << "Total operations: " << (num_operations * num_threads) << std::endl;
        std::cout << "Cache Ratio: " << std::fixed << std::setprecision(2) 
                  << (static_cast<double>(cache_size) / key_space_size * 100) << "%" << std::endl;
        std::cout << std::endl;
        
        std::vector<std::pair<std::string, AccessPattern>> patterns = {
            {"Uniform", AccessPattern::UNIFORM},
            {"Zipf", AccessPattern::ZIPF},
            {"Gaussian", AccessPattern::GAUSSIAN},
            {"Sequential", AccessPattern::SEQUENTIAL},
            {"Sequential+Jump", AccessPattern::SEQUENTIAL_WITH_JUMP},
            {"Exponential", AccessPattern::EXPONENTIAL}
        };
        
        // Test all policies with all patterns
        for (const auto& pattern : patterns) {
            std::cout << "Testing pattern: " << pattern.first << std::endl;
            
            if (num_threads == 1) {
                // Single-threaded tests
                // LRU
                results_.push_back(run_single_test<LRUCacheManager<uint64_t, uint64_t>>(
                    "LRU", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // LFU
                results_.push_back(run_single_test<LFUCacheManager<uint64_t, uint64_t>>(
                    "LFU", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // FIFO
                results_.push_back(run_single_test<FIFOCacheManager<uint64_t, uint64_t>>(
                    "FIFO", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // CLOCK
                results_.push_back(run_single_test<CLOCKCacheManager<uint64_t, uint64_t>>(
                    "CLOCK", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // CLOCK_FREQ
                results_.push_back(run_single_test<CLOCK_FREQCacheManager<uint64_t, uint64_t>>(
                    "CLOCK_FREQ", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // SIEVE
                results_.push_back(run_single_test<SIEVECacheManager<uint64_t, uint64_t>>(
                    "SIEVE", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
                
                // ARC
                results_.push_back(run_single_test<ARCCacheManager<uint64_t, uint64_t>>(
                    "ARC", pattern.first, cache_size, key_space_size, num_operations, pattern.second, report_interval));
            } 
            else {
                // Multi-threaded tests
                // LRU
                results_.push_back(run_multi_threaded_test<LRUCacheManager<uint64_t, uint64_t>>(
                    "LRU", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // LFU
                results_.push_back(run_multi_threaded_test<LFUCacheManager<uint64_t, uint64_t>>(
                    "LFU", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // FIFO
                results_.push_back(run_multi_threaded_test<FIFOCacheManager<uint64_t, uint64_t>>(
                    "FIFO", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // CLOCK
                results_.push_back(run_multi_threaded_test<CLOCKCacheManager<uint64_t, uint64_t>>(
                    "CLOCK", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // CLOCK_FREQ
                results_.push_back(run_multi_threaded_test<CLOCK_FREQCacheManager<uint64_t, uint64_t>>(
                    "CLOCK_FREQ", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // SIEVE
                results_.push_back(run_multi_threaded_test<SIEVECacheManager<uint64_t, uint64_t>>(
                    "SIEVE", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
                
                // ARC
                results_.push_back(run_multi_threaded_test<ARCCacheManager<uint64_t, uint64_t>>(
                    "ARC", pattern.first, cache_size, key_space_size, num_operations, pattern.second, num_threads, report_interval));
            }
        }
    }
    
    void print_results() {
        std::cout << "\n=== Performance Results ===" << std::endl;
        
        // Group results by pattern
        std::map<std::string, std::vector<TestResult>> pattern_results;
        for (const auto& result : results_) {
            pattern_results[result.pattern_name].push_back(result);
        }
        
        for (const auto& pattern_group : pattern_results) {
            std::cout << "\n--- " << pattern_group.first << " Pattern ---" << std::endl;
            
            // Sort by miss ratio (worst to best)
            auto sorted_results = pattern_group.second;
            std::sort(sorted_results.begin(), sorted_results.end(), 
                     [](const TestResult& a, const TestResult& b) {
                         return a.miss_ratio > b.miss_ratio;
                     });
            
            std::cout << std::left << std::setw(8) << "Policy" 
                      << std::setw(8) << "Hit%" 
                      << std::setw(8) << "Miss%" 
                      << std::setw(12) << "Evictions"
                      << std::setw(15) << "Throughput"
                      << std::setw(15) << "Avg Time(ns)" << std::endl;
            std::cout << std::string(70, '-') << std::endl;
            
            for (const auto& result : sorted_results) {
                std::cout << std::left << std::setw(8) << result.policy_name
                          << std::fixed << std::setprecision(2)
                          << std::setw(8) << (result.hit_ratio * 100)
                          << std::setw(8) << (result.miss_ratio * 100)
                          << std::setw(12) << result.evictions
                          << std::setw(15) << std::setprecision(0) << result.throughput_ops_per_sec
                          << std::setw(15) << std::setprecision(2) << result.avg_operation_time_ns << std::endl;
            }
        }
        
        // Summary: Best policy for each pattern
        std::cout << "\n=== Best Policy by Pattern ===" << std::endl;
        for (const auto& pattern_group : pattern_results) {
            auto best_policy = *std::min_element(pattern_group.second.begin(), pattern_group.second.end(),
                                               [](const TestResult& a, const TestResult& b) {
                                                   return a.miss_ratio < b.miss_ratio;
                                               });
            std::cout << pattern_group.first << ": " << best_policy.policy_name 
                      << " (Miss Rate: " << std::fixed << std::setprecision(2) 
                      << (best_policy.miss_ratio * 100) << "%)" << std::endl;
        }
    }
    
    void print_detailed_analysis() {
        std::cout << "\n=== Detailed Analysis ===" << std::endl;
        
        // Overall best and worst policies
        auto best_overall = *std::min_element(results_.begin(), results_.end(),
                                            [](const TestResult& a, const TestResult& b) {
                                                return a.miss_ratio < b.miss_ratio;
                                            });
        
        auto worst_overall = *std::max_element(results_.begin(), results_.end(),
                                             [](const TestResult& a, const TestResult& b) {
                                                 return a.miss_ratio < b.miss_ratio;
                                             });
        
        std::cout << "Best Overall: " << best_overall.policy_name << " with " 
                  << best_overall.pattern_name << " pattern (Miss Rate: " 
                  << std::fixed << std::setprecision(2) << (best_overall.miss_ratio * 100) << "%)" << std::endl;
        
        std::cout << "Worst Overall: " << worst_overall.policy_name << " with " 
                  << worst_overall.pattern_name << " pattern (Miss Rate: " 
                  << std::fixed << std::setprecision(2) << (worst_overall.miss_ratio * 100) << "%)" << std::endl;
        
        // Policy performance summary
        std::map<std::string, std::vector<double>> policy_miss_rates;
        for (const auto& result : results_) {
            policy_miss_rates[result.policy_name].push_back(result.miss_ratio);
        }
        
        std::cout << "\nPolicy Performance Summary (Average Miss Rate):" << std::endl;
        for (const auto& policy : policy_miss_rates) {
            double avg_miss_rate = std::accumulate(policy.second.begin(), policy.second.end(), 0.0) 
                                  / policy.second.size();
            std::cout << policy.first << ": " << std::fixed << std::setprecision(2) 
                      << (avg_miss_rate * 100) << "%" << std::endl;
        }
    }
};

// ============================================================================
// Main Function
// ============================================================================

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c, --cache-size SIZE     Cache size (default: 1000000)" << std::endl;
    std::cout << "  -k, --key-space SIZE      Key space size (default: 10000000)" << std::endl;
    std::cout << "  -o, --operations NUM      Number of operations per thread (default: 5000000)" << std::endl;
    std::cout << "  -t, --threads NUM         Number of threads (default: 1)" << std::endl;
    std::cout << "  -r, --report-interval NUM Statistics report interval (default: 10000)" << std::endl;
    std::cout << "  -h, --help                Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Example: " << program_name << " -c 500000 -k 5000000 -o 2000000 -t 4 -r 5000" << std::endl;
}

int main(int argc, char* argv[]) {
    uint64_t cache_size = 1000000;
    uint64_t key_space_size = 10000000;
    uint64_t num_operations = 5000000;
    uint64_t num_threads = 1;
    uint64_t report_interval = 10000;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--cache-size") {
            if (++i < argc) cache_size = std::stoull(argv[i]);
        } else if (arg == "-k" || arg == "--key-space") {
            if (++i < argc) key_space_size = std::stoull(argv[i]);
        } else if (arg == "-o" || arg == "--operations") {
            if (++i < argc) num_operations = std::stoull(argv[i]);
        } else if (arg == "-t" || arg == "--threads") {
            if (++i < argc) num_threads = std::stoull(argv[i]);
        } else if (arg == "-r" || arg == "--report-interval") {
            if (++i < argc) report_interval = std::stoull(argv[i]);
        }
    }
    
    try {
        CachePerformanceTester tester;
        tester.run_comprehensive_test(cache_size, key_space_size, num_operations, report_interval, num_threads);
        tester.print_results();
        tester.print_detailed_analysis();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 