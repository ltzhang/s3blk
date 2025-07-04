// SPDX-License-Identifier: MIT or GPL-2.0-only

// NOTE: All EntryType definitions in policy_traits.h must include: int pin_count = 0;

#ifndef TEMPLATE_CACHE_MANAGER_HPP
#define TEMPLATE_CACHE_MANAGER_HPP

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <vector>
#include <iostream>
#include <type_traits>
#include "policy_traits.h"

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
class TemplateCacheManager {
public:
    using Policy = PolicyTemplate<Key, Value>;
    using EntryType = typename Policy::EntryType;
    using ManagerDataType = typename Policy::ManagerDataType;
    using KeyType = typename EntryType::key_type;
    using ValueType = typename EntryType::value_type;
    
    // Constructor and destructor
    explicit TemplateCacheManager(uint64_t cache_size, bool debug = false);
    ~TemplateCacheManager();
    
    // Disable copy constructor and assignment operator
    TemplateCacheManager(const TemplateCacheManager&) = delete;
    TemplateCacheManager& operator=(const TemplateCacheManager&) = delete;
    
    // Core cache operations
    bool lookup(const KeyType& key, ValueType& value);
    bool insert(const KeyType& key, const ValueType& value);
    void mark_dirty(const KeyType& key);
    void mark_clean(const KeyType& key);
    void invalidate(const KeyType& key);
    
    // Cache management
    void resize(uint64_t new_cache_size);
    void clear();
    
    // Statistics
    double get_hit_ratio() const;
    uint64_t get_hits() const;
    uint64_t get_misses() const;
    uint64_t get_evictions() const;
    uint64_t get_used_entries() const;
    uint64_t get_cache_size() const;
    
    // Debug and monitoring
    void print_stats() const;
    void print_state() const;
    void set_debug(bool debug) { debug_ = debug; }

    void pin(const KeyType& key);
    void unpin(const KeyType& key);
    std::vector<KeyType> get_dirty(size_t n) const;

    // Get policy name for testing
    const char* get_policy_name() const { return Policy::name(); }

private:
    // Helper methods
    EntryType* evict_entry();
    EntryType* create_entry(const KeyType& key, const ValueType& value, uint32_t index);
    bool can_evict(const EntryType& entry) const;
    void debug_print(const std::string& operation, const KeyType& key = KeyType{}) const;
    
    // Member variables
    std::unordered_map<KeyType, uint32_t> cache_map_;  // key -> index in entries vector
    std::vector<EntryType> entries_;  // All entries (both valid and invalid)
    std::vector<uint32_t> free_indices_;  // Stack of free indices
    ManagerDataType policy_data_;
    mutable std::mutex mutex_;
    
    uint64_t cache_size_;
    uint64_t used_entries_;
    bool debug_;
    
    // Cache statistics
    uint64_t hits_;
    uint64_t misses_;
    uint64_t evictions_;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
TemplateCacheManager<Key, Value, PolicyTemplate>::TemplateCacheManager(uint64_t cache_size, bool debug)
    : cache_size_(cache_size), used_entries_(0),
      hits_(0), misses_(0), evictions_(0), debug_(debug) {
    
    // Pre-allocate entries vector
    entries_.resize(cache_size);
    free_indices_.reserve(cache_size);
    
    for (uint32_t i = 0; i < cache_size; ++i) {
        entries_[i].valid = false;  // Mark as invalid initially
        free_indices_.push_back(cache_size - 1 - i);  // Reverse order for stack behavior
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
TemplateCacheManager<Key, Value, PolicyTemplate>::~TemplateCacheManager() {
    clear();
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
typename TemplateCacheManager<Key, Value, PolicyTemplate>::EntryType* 
TemplateCacheManager<Key, Value, PolicyTemplate>::create_entry(const KeyType& key, const ValueType& value, uint32_t index) {
    entries_[index] = EntryType(key, value, index);
    return &entries_[index];
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
bool TemplateCacheManager<Key, Value, PolicyTemplate>::can_evict(const EntryType& entry) const {
    return entry.valid && entry.pin_count == 0 && !entry.dirty;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::debug_print(const std::string& operation, const KeyType& key) const {
    if (!debug_) return;
    
    std::cout << "\n=== DEBUG: " << operation;
    if (key != KeyType{}) {
        std::cout << " (key=" << key << ")";
    }
    std::cout << " ===\n";
    std::cout << "Used entries: " << used_entries_ << "/" << cache_size_ << "\n";
    std::cout << "Cache contents:\n";
    
    for (const auto& pair : cache_map_) {
        const EntryType& entry = entries_[pair.second];
        if (entry.valid) {
            std::cout << "  " << pair.first << " -> " << entry.value 
                      << " (index: " << entry.index
                      << ", pin_count: " << entry.pin_count
                      << ", dirty: " << (entry.dirty ? "yes" : "no") << ")\n";
        }
    }
    
    if constexpr (std::is_same_v<PolicyTemplate<Key, Value>, LRU<Key, Value>>) {
        std::cout << "LRU list: ";
        int idx = policy_data_.head;
        while (idx != -1) {
            const EntryType& entry = entries_[idx];
            std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << ")";
            if (entry.next != -1) std::cout << " -> ";
            idx = entry.next;
        }
        std::cout << "\n";
    }
    std::cout << "========================\n" << std::flush;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
typename TemplateCacheManager<Key, Value, PolicyTemplate>::EntryType* 
TemplateCacheManager<Key, Value, PolicyTemplate>::evict_entry() {
    // Policy-agnostic: ask the policy for an evictable candidate
    auto can_evict_pred = [this](const EntryType& entry) { return can_evict(entry); };
    for (size_t attempts = 0; attempts < entries_.size(); ++attempts) {
        EntryType* candidate = Policy::get_eviction_candidate(policy_data_, entries_, can_evict_pred);
        if (!candidate) return nullptr;
        // Evict the candidate
        candidate->valid = false;
        used_entries_--;
        evictions_++;
        Policy::on_remove(policy_data_, *candidate, entries_);
        free_indices_.push_back(candidate->index);
        
        // For CLOCK policy, rebuild the list to ensure consistency
        // Note: This is a CLOCK-specific optimization to maintain list consistency
        // Other policies don't need this as they maintain their own invariants
        
        return candidate;
    }
    return nullptr;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
bool TemplateCacheManager<Key, Value, PolicyTemplate>::lookup(const KeyType& key, ValueType& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        EntryType& entry = entries_[it->second];
        value = entry.value;
        Policy::on_access(policy_data_, entry, entries_);
        hits_++;
        debug_print("LOOKUP", key);
        return true;
    }
    
    misses_++;
    debug_print("LOOKUP_MISS", key);
    return false;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
bool
TemplateCacheManager<Key, Value, PolicyTemplate>::insert(const KeyType& key, const ValueType& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if already exists
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        Policy::on_access(policy_data_, entries_[it->second], entries_);
        debug_print("INSERT_EXISTS", key);
        return false;
    }
    
    if (debug_) {
        std::cout << "[INSERT] used_entries_=" << used_entries_ << ", cache_size_=" << cache_size_ << std::endl;
    }
    // Need to evict if cache is full
    if (used_entries_ >= cache_size_) {
        if (debug_) {
            std::cout << "[INSERT] Cache full, attempting eviction for key=" << key << std::endl;
        }
        EntryType* evicted = evict_entry();
        if (!evicted) {
            debug_print("INSERT_FAIL_NO_EVICT", key);
            return false; // Cache full and no entry to evict
        }
        
        // Remove from map
        cache_map_.erase(evicted->key);
    }
    
    // Get free index
    if (free_indices_.empty()) {
        debug_print("INSERT_FAIL_NO_FREE", key);
        return false; // No free indices
    }
    
    uint32_t index = free_indices_.back();
    free_indices_.pop_back();
    
    // Create new entry
    EntryType* entry = create_entry(key, value, index);
    
    // Add to map and policy
    cache_map_[key] = index;
    Policy::on_insert(policy_data_, *entry, entries_);
    
    used_entries_++;
    
    debug_print("INSERT", key);
    return true;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::mark_dirty(const KeyType& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        entries_[it->second].dirty = true;
        debug_print("MARK_DIRTY", key);
    } else {
        debug_print("MARK_DIRTY_FAIL", key);
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::mark_clean(const KeyType& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        entries_[it->second].dirty = false;
        debug_print("MARK_CLEAN", key);
    } else {
        debug_print("MARK_CLEAN_FAIL", key);
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::invalidate(const KeyType& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        EntryType& entry = entries_[it->second];
        
        // Remove from policy
        Policy::on_remove(policy_data_, entry, entries_);
        
        entry.valid = false;
        used_entries_--;
        
        // Return index to free pool
        free_indices_.push_back(entry.index);
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::resize(uint64_t new_cache_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_size_ = new_cache_size;
    
    // Evict entries if new size is smaller
    while (used_entries_ > cache_size_) {
        EntryType* evicted = evict_entry();
        if (evicted) {
            cache_map_.erase(evicted->key);
        } else {
            break;
        }
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    cache_map_.clear();
    free_indices_.clear();
    
    // Reset policy data
    policy_data_ = ManagerDataType();
    
    // Reset all entries
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        entries_[i].valid = false;
    }
    
    // Rebuild free indices
    for (uint32_t i = 0; i < cache_size_; ++i) {
        free_indices_.push_back(cache_size_ - 1 - i);
    }
    
    used_entries_ = 0;
    hits_ = misses_ = evictions_ = 0;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
double TemplateCacheManager<Key, Value, PolicyTemplate>::get_hit_ratio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = hits_ + misses_;
    return total > 0 ? static_cast<double>(hits_) / total : 0.0;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
uint64_t TemplateCacheManager<Key, Value, PolicyTemplate>::get_hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
uint64_t TemplateCacheManager<Key, Value, PolicyTemplate>::get_misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
uint64_t TemplateCacheManager<Key, Value, PolicyTemplate>::get_evictions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evictions_;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
uint64_t TemplateCacheManager<Key, Value, PolicyTemplate>::get_used_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_entries_;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
uint64_t TemplateCacheManager<Key, Value, PolicyTemplate>::get_cache_size() const {
    return cache_size_;
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::print_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "Cache Statistics:\n";
    std::cout << "  Policy: " << Policy::name() << "\n";
    std::cout << "  Cache size: " << cache_size_ << " entries\n";
    std::cout << "  Used entries: " << used_entries_ << "\n";
    std::cout << "  Hits: " << hits_ << "\n";
    std::cout << "  Misses: " << misses_ << "\n";
    std::cout << "  Evictions: " << evictions_ << "\n";
    std::cout << "  Hit ratio: " << (get_hit_ratio() * 100.0) << "%\n";
    Policy::print_stats(policy_data_, entries_);
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::print_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "Cache State:\n";
    std::cout << "  Policy: " << Policy::name() << "\n";
    std::cout << "  Used entries: " << used_entries_ << "/" << cache_size_ << "\n";
    std::cout << "  Cache entries:\n";
    
    for (const auto& pair : cache_map_) {
        const EntryType& entry = entries_[pair.second];
        if (entry.valid) {
            std::cout << "    " << pair.first << " -> " << entry.value 
                      << " (index: " << entry.index
                      << ", pin_count: " << entry.pin_count
                      << ", dirty: " << (entry.dirty ? "yes" : "no")
                      << Policy::get_entry_info(entry) << ")\n";
        }
    }
    
    Policy::print_state(policy_data_, entries_);
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::pin(const KeyType& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        entries_[it->second].pin_count++;
        debug_print("PIN", key);
    } else {
        debug_print("PIN_FAIL", key);
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
void TemplateCacheManager<Key, Value, PolicyTemplate>::unpin(const KeyType& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end() && entries_[it->second].valid) {
        if (entries_[it->second].pin_count > 0) {
            entries_[it->second].pin_count--;
            debug_print("UNPIN", key);
        } else {
            debug_print("UNPIN_FAIL_ALREADY_UNPINNED", key);
        }
    } else {
        debug_print("UNPIN_FAIL_NOT_FOUND", key);
    }
}

template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
std::vector<typename TemplateCacheManager<Key, Value, PolicyTemplate>::KeyType>
TemplateCacheManager<Key, Value, PolicyTemplate>::get_dirty(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<typename TemplateCacheManager<Key, Value, PolicyTemplate>::KeyType> dirty_keys;
    for (const auto& pair : cache_map_) {
        const EntryType& entry = entries_[pair.second];
        if (entry.valid && entry.dirty) {
            dirty_keys.push_back(pair.first);
            if (dirty_keys.size() >= n) break;
        }
    }
    return dirty_keys;
}

// ============================================================================
// Type Aliases for Easy Use
// ============================================================================

// For general key-value caching with specific policies
template<typename Key, typename Value>
using LRUCacheManager = TemplateCacheManager<Key, Value, LRU>;

template<typename Key, typename Value>
using LFUCacheManager = TemplateCacheManager<Key, Value, LFU>;

template<typename Key, typename Value>
using FIFOCacheManager = TemplateCacheManager<Key, Value, FIFO>;

template<typename Key, typename Value>
using CLOCKCacheManager = TemplateCacheManager<Key, Value, CLOCK>;

template<typename Key, typename Value>
using SIEVECacheManager = TemplateCacheManager<Key, Value, SIEVE>;

template<typename Key, typename Value>
using ARCCacheManager = TemplateCacheManager<Key, Value, ARC>;

#endif // TEMPLATE_CACHE_MANAGER_HPP 