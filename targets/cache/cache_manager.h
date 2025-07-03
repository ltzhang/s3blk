// SPDX-License-Identifier: MIT or GPL-2.0-only

#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Cache entry representing a logical sector in the cache
struct cache_entry {
    uint64_t logical_sector;    // Logical sector number
    uint64_t physical_sector;   // Physical sector in cache file
    bool dirty;                 // Whether the entry has been modified
    bool valid;                 // Whether the entry contains valid data
    uint64_t last_access;       // Last access time for LRU
    uint64_t access_count;      // Access count for LFU
    struct cache_entry *next;   // Next entry in list
    struct cache_entry *prev;   // Previous entry in list
};

// Cache eviction policy types
enum eviction_policy {
    EVICTION_LRU,    // Least Recently Used
    EVICTION_LFU,    // Least Frequently Used
    EVICTION_FIFO    // First In First Out
};

// Cache manager structure
struct cache_manager {
    struct cache_entry **hash_table;  // Hash table for quick lookups
    struct cache_entry *lru_head;     // Head of LRU list
    struct cache_entry *lru_tail;     // Tail of LRU list
    pthread_mutex_t mutex;            // Mutex for thread safety
    
    uint64_t cache_size_sectors;      // Total cache size in sectors
    uint64_t used_sectors;            // Currently used sectors
    uint64_t access_counter;          // Global access counter for LRU
    enum eviction_policy policy;      // Current eviction policy
    uint64_t hash_size;               // Size of hash table
    
    // Cache statistics
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
};

// Function declarations
struct cache_manager *cache_manager_create(uint64_t cache_size_sectors, enum eviction_policy policy);
void cache_manager_destroy(struct cache_manager *cm);

// Core cache operations
bool cache_lookup(struct cache_manager *cm, uint64_t logical_sector, uint64_t *physical_sector);
uint64_t cache_insert(struct cache_manager *cm, uint64_t logical_sector);
void cache_mark_dirty(struct cache_manager *cm, uint64_t logical_sector);
void cache_mark_clean(struct cache_manager *cm, uint64_t logical_sector);
void cache_invalidate(struct cache_manager *cm, uint64_t logical_sector);

// Cache management
void cache_set_policy(struct cache_manager *cm, enum eviction_policy new_policy);
void cache_resize(struct cache_manager *cm, uint64_t new_cache_size_sectors);
void cache_clear(struct cache_manager *cm);

// Statistics
double cache_get_hit_ratio(struct cache_manager *cm);
uint64_t cache_get_hits(struct cache_manager *cm);
uint64_t cache_get_misses(struct cache_manager *cm);
uint64_t cache_get_evictions(struct cache_manager *cm);
uint64_t cache_get_used_sectors(struct cache_manager *cm);
uint64_t cache_get_cache_size(struct cache_manager *cm);

// Debug and monitoring
void cache_print_stats(struct cache_manager *cm);
void cache_print_state(struct cache_manager *cm);

#endif // CACHE_MANAGER_H 