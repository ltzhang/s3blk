// SPDX-License-Identifier: MIT or GPL-2.0-only

#include "cache_manager.h"
#include <iostream>
#include <string>

int main() {
    std::cout << "=== TemplateCacheManager Usage Examples ===\n\n";
    
    // Example 1: int key, int value with LRU policy
    std::cout << "1. Int-Int cache with LRU policy:\n";
    TemplateCacheManager<int, int, LRU> int_cache(3);
    
    int_cache.insert(1, 100);
    int_cache.insert(2, 200);
    int_cache.insert(3, 300);
    
    int value;
    if (int_cache.lookup(1, value)) {
        std::cout << "Found key 1: " << value << "\n";
    }
    
    // This will evict the least recently used entry (key 2)
    int_cache.insert(4, 400);
    
    int_cache.print_stats();
    std::cout << "\n";
    
    // Example 2: string key, string value with LFU policy
    std::cout << "2. String-String cache with LFU policy:\n";
    TemplateCacheManager<std::string, std::string, LFU> string_cache(2);
    
    string_cache.insert("hello", "world");
    string_cache.insert("foo", "bar");
    
    std::string str_value;
    if (string_cache.lookup("hello", str_value)) {
        std::cout << "Found 'hello': " << str_value << "\n";
    }
    
    // Access 'hello' again to increase its frequency
    string_cache.lookup("hello", str_value);
    
    // This will evict the least frequently used entry ('foo')
    string_cache.insert("baz", "qux");
    
    string_cache.print_stats();
    std::cout << "\n";
    
    // Example 3: Using the convenient type aliases
    std::cout << "3. Using convenient type aliases:\n";
    LRUCacheManager<int, std::string> alias_cache(2);
    
    alias_cache.insert(1, "one");
    alias_cache.insert(2, "two");
    alias_cache.insert(3, "three"); // Will evict key 1
    
    std::string alias_value;
    if (alias_cache.lookup(1, alias_value)) {
        std::cout << "Key 1 still exists: " << alias_value << "\n";
    } else {
        std::cout << "Key 1 was evicted\n";
    }
    
    if (alias_cache.lookup(2, alias_value)) {
        std::cout << "Key 2 exists: " << alias_value << "\n";
    }
    
    alias_cache.print_stats();
    
    return 0;
} 