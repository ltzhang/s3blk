// SPDX-License-Identifier: MIT or GPL-2.0-only

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "cache_manager.h"

#define HASH_SIZE 1024
#define HASH_MASK (HASH_SIZE - 1)

static uint64_t hash_sector(uint64_t sector)
{
    return sector & HASH_MASK;
}

static uint64_t get_current_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static struct cache_entry *find_entry(struct cache_manager *cm, uint64_t logical_sector)
{
    uint64_t hash = hash_sector(logical_sector);
    struct cache_entry *entry = cm->hash_table[hash];
    
    while (entry) {
        if (entry->logical_sector == logical_sector && entry->valid) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

static void remove_from_lru(struct cache_manager *cm, struct cache_entry *entry)
{
    if (!entry) return;
    
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cm->lru_head = entry->next;
    }
    
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cm->lru_tail = entry->prev;
    }
    
    entry->next = entry->prev = NULL;
}

static void add_to_lru_head(struct cache_manager *cm, struct cache_entry *entry)
{
    entry->next = cm->lru_head;
    entry->prev = NULL;
    
    if (cm->lru_head) {
        cm->lru_head->prev = entry;
    } else {
        cm->lru_tail = entry;
    }
    
    cm->lru_head = entry;
}

static void update_lru(struct cache_manager *cm, struct cache_entry *entry)
{
    remove_from_lru(cm, entry);
    add_to_lru_head(cm, entry);
    entry->last_access = get_current_time();
    entry->access_count++;
}

static struct cache_entry *evict_entry(struct cache_manager *cm)
{
    struct cache_entry *entry = NULL;
    
    switch (cm->policy) {
    case EVICTION_LRU:
        entry = cm->lru_tail;
        break;
    case EVICTION_LFU: {
        // Simple LFU: find entry with lowest access count
        entry = cm->lru_head;
        struct cache_entry *current = cm->lru_head;
        while (current) {
            if (current->access_count < entry->access_count) {
                entry = current;
            }
            current = current->next;
        }
        break;
    }
    case EVICTION_FIFO:
        entry = cm->lru_tail;
        break;
    }
    
    if (entry) {
        // Remove from hash table
        uint64_t hash = hash_sector(entry->logical_sector);
        struct cache_entry **pp = &cm->hash_table[hash];
        while (*pp && *pp != entry) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = entry->next;
        }
        
        remove_from_lru(cm, entry);
        entry->valid = false;
        cm->used_sectors--;
        cm->evictions++;
    }
    
    return entry;
}

struct cache_manager *cache_manager_create(uint64_t cache_size_sectors, enum eviction_policy policy)
{
    struct cache_manager *cm = (struct cache_manager *)calloc(1, sizeof(*cm));
    if (!cm) return NULL;
    
    cm->hash_table = (struct cache_entry **)calloc(HASH_SIZE, sizeof(struct cache_entry *));
    if (!cm->hash_table) {
        free(cm);
        return NULL;
    }
    
    cm->cache_size_sectors = cache_size_sectors;
    cm->policy = policy;
    cm->hash_size = HASH_SIZE;
    pthread_mutex_init(&cm->mutex, NULL);
    
    return cm;
}

void cache_manager_destroy(struct cache_manager *cm)
{
    if (!cm) return;
    
    cache_clear(cm);
    
    if (cm->hash_table) {
        free(cm->hash_table);
    }
    
    pthread_mutex_destroy(&cm->mutex);
    free(cm);
}

bool cache_lookup(struct cache_manager *cm, uint64_t logical_sector, uint64_t *physical_sector)
{
    if (!cm || !physical_sector) return false;
    
    pthread_mutex_lock(&cm->mutex);
    
    struct cache_entry *entry = find_entry(cm, logical_sector);
    if (entry) {
        *physical_sector = entry->physical_sector;
        update_lru(cm, entry);
        cm->hits++;
        pthread_mutex_unlock(&cm->mutex);
        return true;
    }
    
    cm->misses++;
    pthread_mutex_unlock(&cm->mutex);
    return false;
}

uint64_t cache_insert(struct cache_manager *cm, uint64_t logical_sector)
{
    if (!cm) return UINT64_MAX;
    
    pthread_mutex_lock(&cm->mutex);
    
    // Check if already exists
    struct cache_entry *entry = find_entry(cm, logical_sector);
    if (entry) {
        update_lru(cm, entry);
        uint64_t physical_sector = entry->physical_sector;
        pthread_mutex_unlock(&cm->mutex);
        return physical_sector;
    }
    
    // Need to evict if cache is full
    if (cm->used_sectors >= cm->cache_size_sectors) {
        entry = evict_entry(cm);
        if (!entry) {
            pthread_mutex_unlock(&cm->mutex);
            return UINT64_MAX;
        }
    } else {
        entry = (struct cache_entry *)calloc(1, sizeof(*entry));
        if (!entry) {
            pthread_mutex_unlock(&cm->mutex);
            return UINT64_MAX;
        }
    }
    
    // Initialize entry
    entry->logical_sector = logical_sector;
    entry->physical_sector = cm->used_sectors;
    entry->valid = true;
    entry->dirty = false;
    entry->last_access = get_current_time();
    entry->access_count = 1;
    
    // Add to hash table
    uint64_t hash = hash_sector(logical_sector);
    entry->next = cm->hash_table[hash];
    cm->hash_table[hash] = entry;
    
    // Add to LRU list
    add_to_lru_head(cm, entry);
    
    cm->used_sectors++;
    
    uint64_t physical_sector = entry->physical_sector;
    pthread_mutex_unlock(&cm->mutex);
    return physical_sector;
}

void cache_mark_dirty(struct cache_manager *cm, uint64_t logical_sector)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    struct cache_entry *entry = find_entry(cm, logical_sector);
    if (entry) {
        entry->dirty = true;
    }
    pthread_mutex_unlock(&cm->mutex);
}

void cache_mark_clean(struct cache_manager *cm, uint64_t logical_sector)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    struct cache_entry *entry = find_entry(cm, logical_sector);
    if (entry) {
        entry->dirty = false;
    }
    pthread_mutex_unlock(&cm->mutex);
}

void cache_invalidate(struct cache_manager *cm, uint64_t logical_sector)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    struct cache_entry *entry = find_entry(cm, logical_sector);
    if (entry) {
        // Remove from hash table
        uint64_t hash = hash_sector(logical_sector);
        struct cache_entry **pp = &cm->hash_table[hash];
        while (*pp && *pp != entry) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            *pp = entry->next;
        }
        
        remove_from_lru(cm, entry);
        entry->valid = false;
        cm->used_sectors--;
    }
    pthread_mutex_unlock(&cm->mutex);
}

void cache_set_policy(struct cache_manager *cm, enum eviction_policy new_policy)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    cm->policy = new_policy;
    pthread_mutex_unlock(&cm->mutex);
}

void cache_resize(struct cache_manager *cm, uint64_t new_cache_size_sectors)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    cm->cache_size_sectors = new_cache_size_sectors;
    
    // Evict entries if new size is smaller
    while (cm->used_sectors > cm->cache_size_sectors) {
        evict_entry(cm);
    }
    pthread_mutex_unlock(&cm->mutex);
}

void cache_clear(struct cache_manager *cm)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    
    // Free all entries
    for (uint64_t i = 0; i < cm->hash_size; i++) {
        struct cache_entry *entry = cm->hash_table[i];
        while (entry) {
            struct cache_entry *next = entry->next;
            free(entry);
            entry = next;
        }
        cm->hash_table[i] = NULL;
    }
    
    cm->lru_head = cm->lru_tail = NULL;
    cm->used_sectors = 0;
    cm->hits = cm->misses = cm->evictions = 0;
    
    pthread_mutex_unlock(&cm->mutex);
}

double cache_get_hit_ratio(struct cache_manager *cm)
{
    if (!cm) return 0.0;
    
    pthread_mutex_lock(&cm->mutex);
    uint64_t total = cm->hits + cm->misses;
    double ratio = total > 0 ? (double)cm->hits / total : 0.0;
    pthread_mutex_unlock(&cm->mutex);
    
    return ratio;
}

uint64_t cache_get_hits(struct cache_manager *cm)
{
    if (!cm) return 0;
    pthread_mutex_lock(&cm->mutex);
    uint64_t hits = cm->hits;
    pthread_mutex_unlock(&cm->mutex);
    return hits;
}

uint64_t cache_get_misses(struct cache_manager *cm)
{
    if (!cm) return 0;
    pthread_mutex_lock(&cm->mutex);
    uint64_t misses = cm->misses;
    pthread_mutex_unlock(&cm->mutex);
    return misses;
}

uint64_t cache_get_evictions(struct cache_manager *cm)
{
    if (!cm) return 0;
    pthread_mutex_lock(&cm->mutex);
    uint64_t evictions = cm->evictions;
    pthread_mutex_unlock(&cm->mutex);
    return evictions;
}

uint64_t cache_get_used_sectors(struct cache_manager *cm)
{
    if (!cm) return 0;
    pthread_mutex_lock(&cm->mutex);
    uint64_t used = cm->used_sectors;
    pthread_mutex_unlock(&cm->mutex);
    return used;
}

uint64_t cache_get_cache_size(struct cache_manager *cm)
{
    if (!cm) return 0;
    return cm->cache_size_sectors;
}

void cache_print_stats(struct cache_manager *cm)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    printf("Cache Statistics:\n");
    printf("  Cache size: %lu sectors\n", cm->cache_size_sectors);
    printf("  Used sectors: %lu\n", cm->used_sectors);
    printf("  Hits: %lu\n", cm->hits);
    printf("  Misses: %lu\n", cm->misses);
    printf("  Evictions: %lu\n", cm->evictions);
    printf("  Hit ratio: %.2f%%\n", cache_get_hit_ratio(cm) * 100.0);
    pthread_mutex_unlock(&cm->mutex);
}

void cache_print_state(struct cache_manager *cm)
{
    if (!cm) return;
    
    pthread_mutex_lock(&cm->mutex);
    printf("Cache State:\n");
    printf("  LRU list: ");
    struct cache_entry *entry = cm->lru_head;
    while (entry) {
        printf("%lu->", entry->logical_sector);
        entry = entry->next;
    }
    printf("NULL\n");
    
    printf("  Hash table:\n");
    for (uint64_t i = 0; i < cm->hash_size; i++) {
        if (cm->hash_table[i]) {
            printf("    [%lu]: ", i);
            entry = cm->hash_table[i];
            while (entry) {
                printf("%lu->", entry->logical_sector);
                entry = entry->next;
            }
            printf("NULL\n");
        }
    }
    pthread_mutex_unlock(&cm->mutex);
} 