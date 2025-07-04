#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <random>
#include <unordered_map>
#include <iomanip>
#include <fstream>
#include <sstream>

// Include cache headers
#include "cache_manager.h"
#include "policy_traits.h"

// Memory measurement utilities
#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>
#endif

class MemoryTracker {
private:
    long initial_memory;
    
public:
    MemoryTracker() {
        initial_memory = get_current_memory();
    }
    
    long get_current_memory() {
#ifdef __linux__
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                std::istringstream iss(line.substr(7));
                long memory_kb;
                iss >> memory_kb;
                return memory_kb * 1024; // Convert KB to bytes
            }
        }
        // Fallback to VmSize if VmRSS not available
        status.clear();
        status.seekg(0);
        while (std::getline(status, line)) {
            if (line.substr(0, 6) == "VmSize:") {
                std::istringstream iss(line.substr(7));
                long memory_kb;
                iss >> memory_kb;
                return memory_kb * 1024; // Convert KB to bytes
            }
        }
#endif
        return 0; // Fallback for non-Linux systems
    }
    
    long get_memory_usage() {
        return get_current_memory() - initial_memory;
    }
    
    void reset() {
        initial_memory = get_current_memory();
    }
};

template<typename CacheType>
class CacheMemoryTest {
private:
    std::unique_ptr<CacheType> cache;
    MemoryTracker memory_tracker;
    std::mt19937 rng;
    
public:
    CacheMemoryTest(uint64_t cache_size) 
        : cache(std::make_unique<CacheType>(cache_size)), rng(std::random_device{}()) {
    }
    
    void run_test(uint64_t key_space_size, int num_rounds, int ops_per_round) {
        std::cout << "Testing " << cache->get_policy_name() 
                  << " with cache_size=" << cache->get_cache_size() 
                  << ", key_space=" << key_space_size 
                  << ", rounds=" << num_rounds 
                  << ", ops_per_round=" << ops_per_round << std::endl;
        
        std::uniform_int_distribution<uint64_t> key_dist(0, key_space_size - 1);
        std::uniform_int_distribution<int> value_dist(1, 1000000);
        std::uniform_real_distribution<double> op_dist(0.0, 1.0);
        
        // Track memory usage over rounds
        std::vector<long> memory_usage;
        std::vector<uint64_t> total_ops;
        
        for (int round = 0; round < num_rounds; ++round) {
            memory_tracker.reset();
            
            // Perform random operations
            for (int op = 0; op < ops_per_round; ++op) {
                uint64_t key = key_dist(rng);
                double op_type = op_dist(rng);
                
                if (op_type < 0.7) {
                    // 70% lookups
                    int value;
                    cache->lookup(key, value);
                } else if (op_type < 0.85) {
                    // 15% inserts
                    int value = value_dist(rng);
                    cache->insert(key, value);
                } else if (op_type < 0.95) {
                    // 10% pin/unpin operations
                    cache->pin(key);
                    if (op % 2 == 0) {
                        cache->unpin(key);
                    }
                } else {
                    // 5% dirty operations
                    cache->mark_dirty(key);
                    if (op % 3 == 0) {
                        cache->mark_clean(key);
                    }
                }
            }
            
            long memory = memory_tracker.get_memory_usage();
            memory_usage.push_back(memory);
            total_ops.push_back((round + 1) * ops_per_round);
            
            std::cout << "  Round " << (round + 1) << ": "
                      << "Memory=" << (memory / 1024) << " KB, "
                      << "Used_entries=" << cache->get_used_entries() << "/" << cache->get_cache_size() << ", "
                      << "Hit_ratio=" << std::fixed << std::setprecision(2) 
                      << (cache->get_hit_ratio() * 100.0) << "%" << std::endl;
        }
        
        // Print memory growth trend
        std::cout << "  Memory growth trend:" << std::endl;
        for (size_t i = 0; i < memory_usage.size(); ++i) {
            std::cout << "    Ops=" << total_ops[i] 
                      << ", Memory=" << (memory_usage[i] / 1024) << " KB" << std::endl;
        }
        
        // Calculate memory per entry
        double avg_memory = 0;
        if (!memory_usage.empty()) {
            avg_memory = static_cast<double>(memory_usage.back()) / cache->get_cache_size();
        }
        
        // Theoretical memory calculation
        size_t entry_size = sizeof(typename CacheType::EntryType);
        size_t cache_map_size = cache->get_used_entries() * sizeof(std::pair<uint64_t, uint32_t>);
        size_t total_theoretical = cache->get_cache_size() * entry_size + cache_map_size;
        
        std::cout << "  Average memory per entry: " << std::fixed << std::setprecision(2) 
                  << avg_memory << " bytes" << std::endl;
        std::cout << "  Final memory usage: " << (memory_usage.back() / 1024) << " KB" << std::endl;
        std::cout << "  Theoretical memory: " << (total_theoretical / 1024) << " KB" << std::endl;
        std::cout << "  Entry size: " << entry_size << " bytes" << std::endl;
        std::cout << "  Cache map size: " << (cache_map_size / 1024) << " KB" << std::endl;
        std::cout << std::endl;
    }
};

int main() {
    std::cout << "=== Cache Memory Overhead Test ===" << std::endl;
    std::cout << "Testing memory usage for different cache policies and sizes" << std::endl;
    std::cout << std::endl;
    
    // Test configurations
    std::vector<uint64_t> cache_sizes = {100, 1000, 10000};
    int num_rounds = 5;
    int ops_per_round = 10000;
    
    // Test each cache policy
    std::vector<std::string> policies = {"LRU", "LFU", "FIFO", "CLOCK", "SIEVE", "ARC"};
    
    for (const auto& policy : policies) {
        std::cout << "=== Testing " << policy << " Policy ===" << std::endl;
        
        for (uint64_t cache_size : cache_sizes) {
            uint64_t key_space = cache_size * 10; // 10x keyspace
            
            if (policy == "LRU") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, LRU>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            } else if (policy == "LFU") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, LFU>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            } else if (policy == "FIFO") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, FIFO>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            } else if (policy == "CLOCK") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, CLOCK>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            } else if (policy == "SIEVE") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, SIEVE>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            } else if (policy == "ARC") {
                CacheMemoryTest<TemplateCacheManager<uint64_t, int, ARC>> test(cache_size);
                test.run_test(key_space, num_rounds, ops_per_round);
            }
        }
        
        std::cout << std::endl;
    }
    
    std::cout << "=== Memory Test Summary ===" << std::endl;
    std::cout << "This test measures:" << std::endl;
    std::cout << "1. Memory overhead per cache entry" << std::endl;
    std::cout << "2. Memory growth patterns over time" << std::endl;
    std::cout << "3. Policy-specific memory characteristics" << std::endl;
    std::cout << "4. CacheManager overhead" << std::endl;
    
    return 0;
} 