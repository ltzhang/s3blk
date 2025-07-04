// SPDX-License-Identifier: MIT or GPL-2.0-only

#include <iostream>
#include <string>
#include <vector>
#include "cache_manager.h"

// Example 1: Sector-based caching (original use case)
void sector_cache_example() {
    std::cout << "=== Sector Cache Example (LRU) ===" << std::endl;
    
    LRUCacheManager cache(5);  // 5 sectors
    
    uint64_t physical_sector;
    
    // Insert some sectors
    cache.insert(100, 1000);
    cache.insert(200, 2000);
    cache.insert(300, 3000);
    
    // Lookup existing
    if (cache.lookup(100, physical_sector)) {
        std::cout << "Found sector 100 -> " << physical_sector << std::endl;
    }
    
    // Insert more to trigger eviction
    cache.insert(400, 4000);
    cache.insert(500, 5000);
    cache.insert(600, 6000);  // This should evict the oldest (100)
    
    // Try to lookup evicted sector
    if (!cache.lookup(100, physical_sector)) {
        std::cout << "Sector 100 was evicted (as expected)" << std::endl;
    }
    
    cache.print_stats();
    std::cout << std::endl;
}

// Example 2: String-based caching
void string_cache_example() {
    std::cout << "=== String Cache Example (LFU) ===" << std::endl;
    
    GenericLFUCache<std::string, std::string> cache(3);
    
    std::string value;
    
    // Insert some key-value pairs
    cache.insert("user1", "John Doe");
    cache.insert("user2", "Jane Smith");
    cache.insert("user3", "Bob Johnson");
    
    // Access some entries multiple times to build frequency
    cache.lookup("user1", value);
    cache.lookup("user1", value);
    cache.lookup("user2", value);
    
    // Insert new entry - should evict least frequently used
    cache.insert("user4", "Alice Brown");
    
    // user3 should be evicted (accessed 0 times)
    if (!cache.lookup("user3", value)) {
        std::cout << "user3 was evicted (least frequently used)" << std::endl;
    }
    
    cache.print_stats();
    std::cout << std::endl;
}

// Example 3: Integer-based caching with FIFO
void integer_cache_example() {
    std::cout << "=== Integer Cache Example (FIFO) ===" << std::endl;
    
    GenericFIFOCache<int, std::vector<int>> cache(3);
    
    std::vector<int> value;
    
    // Insert some data
    cache.insert(1, {1, 2, 3});
    cache.insert(2, {4, 5, 6});
    cache.insert(3, {7, 8, 9});
    
    // Access entries (shouldn't affect FIFO order)
    cache.lookup(1, value);
    cache.lookup(2, value);
    
    // Insert new entry - should evict first in (1)
    cache.insert(4, {10, 11, 12});
    
    if (!cache.lookup(1, value)) {
        std::cout << "Key 1 was evicted (FIFO order)" << std::endl;
    }
    
    cache.print_stats();
    std::cout << std::endl;
}

// Example 4: Custom type caching with CLOCK
struct CustomKey {
    int id;
    std::string name;
    
    bool operator==(const CustomKey& other) const {
        return id == other.id && name == other.name;
    }
};

struct CustomValue {
    double score;
    std::vector<std::string> tags;
    
    CustomValue() : score(0.0) {}
    CustomValue(double s, const std::vector<std::string>& t) : score(s), tags(t) {}
};

// Hash function for CustomKey
namespace std {
    template<>
    struct hash<CustomKey> {
        size_t operator()(const CustomKey& k) const {
            return hash<int>()(k.id) ^ hash<string>()(k.name);
        }
    };
}

void custom_type_cache_example() {
    std::cout << "=== Custom Type Cache Example (CLOCK) ===" << std::endl;
    
    GenericCLOCKCache<CustomKey, CustomValue> cache(3);
    
    CustomValue value;
    
    // Insert some custom data
    cache.insert({1, "item1"}, {95.5, {"tag1", "tag2"}});
    cache.insert({2, "item2"}, {87.2, {"tag3"}});
    cache.insert({3, "item3"}, {92.1, {"tag1", "tag4"}});
    
    // Access some entries to set reference bits
    cache.lookup({1, "item1"}, value);
    cache.lookup({2, "item2"}, value);
    
    // Insert new entry - should evict based on CLOCK algorithm
    cache.insert({4, "item4"}, {88.9, {"tag2", "tag5"}});
    
    // item3 should be evicted (not accessed, reference bit = false)
    if (!cache.lookup({3, "item3"}, value)) {
        std::cout << "item3 was evicted (CLOCK algorithm)" << std::endl;
    }
    
    cache.print_stats();
    std::cout << std::endl;
}

int main() {
    std::cout << "Flexible Cache System Examples\n";
    std::cout << "==============================\n\n";
    
    sector_cache_example();
    string_cache_example();
    integer_cache_example();
    custom_type_cache_example();
    
    std::cout << "All examples completed successfully!" << std::endl;
    return 0;
} 