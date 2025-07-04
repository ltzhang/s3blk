// SPDX-License-Identifier: MIT or GPL-2.0-only

#ifndef POLICY_TRAITS_HPP
#define POLICY_TRAITS_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include <type_traits>

// Base entry for all policies - templated on Key and Value types
template<typename Key, typename Value>
struct CacheEntry {
    using key_type = Key;
    using value_type = Value;
    
    Key key;
    Value value;
    bool dirty;
    bool valid;
    uint32_t index;
    
    CacheEntry() : key(), value(), dirty(false), valid(false), index(0) {}
    CacheEntry(const Key& k, const Value& v, uint32_t idx)
        : key(k), value(v), dirty(false), valid(true), index(idx) {}
};

// ===================== LRU Policy =====================
template<typename Key, typename Value>
class LRU {
public:
    struct Entry : public CacheEntry<Key, Value> {
        int prev, next;
        Entry() : CacheEntry<Key, Value>(), prev(-1), next(-1) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), prev(-1), next(-1) {}
    };
    
    struct ManagerData {
        int head = -1, tail = -1;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (m.head == (int)e.index) return; // already at head
        // Remove from current position
        if (e.prev != -1) entries[e.prev].next = e.next;
        if (e.next != -1) entries[e.next].prev = e.prev;
        if (m.tail == (int)e.index) m.tail = e.prev;
        // Insert at head
        e.prev = -1;
        e.next = m.head;
        if (m.head != -1) entries[m.head].prev = e.index;
        m.head = e.index;
        if (m.tail == -1) m.tail = e.index;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        e.prev = -1;
        e.next = m.head;
        if (m.head != -1) entries[m.head].prev = e.index;
        m.head = e.index;
        if (m.tail == -1) m.tail = e.index;
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.prev != -1) entries[e.prev].next = e.next;
        if (e.next != -1) entries[e.next].prev = e.prev;
        if (m.head == (int)e.index) m.head = e.next;
        if (m.tail == (int)e.index) m.tail = e.prev;
        e.prev = e.next = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries) {
        if (m.tail == -1) return nullptr;
        return &entries[m.tail];
    }
    
    static const char* name() { return "LRU"; }
};

// ===================== LFU Policy =====================
template<typename Key, typename Value>
class LFU {
public:
    struct Entry : public CacheEntry<Key, Value> {
        uint64_t access_count;
        int next_in_bucket, prev_in_bucket;
        Entry() : CacheEntry<Key, Value>(), access_count(0), next_in_bucket(-1), prev_in_bucket(-1) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), access_count(1), next_in_bucket(-1), prev_in_bucket(-1) {}
    };
    
    struct ManagerData {
        std::unordered_map<uint64_t, int> bucket_heads; // count -> index
        std::unordered_map<uint64_t, int> bucket_tails; // count -> index
        uint64_t min_count = 1;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        // Remove from old bucket
        uint64_t old_count = e.access_count;
        if (e.prev_in_bucket != -1) entries[e.prev_in_bucket].next_in_bucket = e.next_in_bucket;
        if (e.next_in_bucket != -1) entries[e.next_in_bucket].prev_in_bucket = e.prev_in_bucket;
        if (m.bucket_heads[old_count] == (int)e.index) m.bucket_heads[old_count] = e.next_in_bucket;
        if (m.bucket_tails[old_count] == (int)e.index) m.bucket_tails[old_count] = e.prev_in_bucket;
        // If bucket is empty, erase
        if (m.bucket_heads[old_count] == -1) {
            m.bucket_heads.erase(old_count);
            m.bucket_tails.erase(old_count);
            if (m.min_count == old_count) m.min_count++;
        }
        // Add to new bucket
        e.access_count++;
        e.prev_in_bucket = -1;
        e.next_in_bucket = m.bucket_heads.count(e.access_count) ? m.bucket_heads[e.access_count] : -1;
        if (e.next_in_bucket != -1) entries[e.next_in_bucket].prev_in_bucket = e.index;
        m.bucket_heads[e.access_count] = e.index;
        if (!m.bucket_tails.count(e.access_count)) m.bucket_tails[e.access_count] = e.index;
        if (m.min_count > e.access_count || m.bucket_heads.empty()) m.min_count = e.access_count;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        e.access_count = 1;
        e.prev_in_bucket = -1;
        e.next_in_bucket = m.bucket_heads.count(1) ? m.bucket_heads[1] : -1;
        if (e.next_in_bucket != -1) entries[e.next_in_bucket].prev_in_bucket = e.index;
        m.bucket_heads[1] = e.index;
        if (!m.bucket_tails.count(1)) m.bucket_tails[1] = e.index;
        m.min_count = 1;
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        uint64_t count = e.access_count;
        if (e.prev_in_bucket != -1) entries[e.prev_in_bucket].next_in_bucket = e.next_in_bucket;
        if (e.next_in_bucket != -1) entries[e.next_in_bucket].prev_in_bucket = e.prev_in_bucket;
        if (m.bucket_heads[count] == (int)e.index) m.bucket_heads[count] = e.next_in_bucket;
        if (m.bucket_tails[count] == (int)e.index) m.bucket_tails[count] = e.prev_in_bucket;
        if (m.bucket_heads[count] == -1) {
            m.bucket_heads.erase(count);
            m.bucket_tails.erase(count);
            if (m.min_count == count) m.min_count++;
        }
        e.prev_in_bucket = e.next_in_bucket = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries) {
        if (m.bucket_heads.empty()) return nullptr;
        int idx = m.bucket_heads[m.min_count];
        return idx == -1 ? nullptr : &entries[idx];
    }
    
    static const char* name() { return "LFU"; }
};

// ===================== FIFO Policy =====================
template<typename Key, typename Value>
class FIFO {
public:
    struct Entry : public CacheEntry<Key, Value> {
        int prev, next;
        Entry() : CacheEntry<Key, Value>(), prev(-1), next(-1) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), prev(-1), next(-1) {}
    };
    
    struct ManagerData {
        int head = -1, tail = -1;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData&, Entry&, std::vector<Entry>&) {}
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        e.prev = m.tail;
        e.next = -1;
        if (m.tail != -1) entries[m.tail].next = e.index;
        m.tail = e.index;
        if (m.head == -1) m.head = e.index;
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.prev != -1) entries[e.prev].next = e.next;
        if (e.next != -1) entries[e.next].prev = e.prev;
        if (m.head == (int)e.index) m.head = e.next;
        if (m.tail == (int)e.index) m.tail = e.prev;
        e.prev = e.next = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries) {
        if (m.head == -1) return nullptr;
        return &entries[m.head];
    }
    
    static const char* name() { return "FIFO"; }
};

// ===================== CLOCK Policy =====================
template<typename Key, typename Value>
class CLOCK {
public:
    struct Entry : public CacheEntry<Key, Value> {
        int next;
        bool reference_bit;
        Entry() : CacheEntry<Key, Value>(), next(-1), reference_bit(false) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), next(-1), reference_bit(true) {}
    };
    
    struct ManagerData {
        int hand = -1;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData&, Entry& e, std::vector<Entry>&) {
        e.reference_bit = true;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (m.hand == -1) {
            m.hand = e.index;
            e.next = e.index;
        } else {
            int tail = m.hand;
            while (entries[tail].next != m.hand) tail = entries[tail].next;
            entries[tail].next = e.index;
            e.next = m.hand;
        }
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.next == e.index) {
            m.hand = -1;
        } else {
            int prev = m.hand;
            while (entries[prev].next != e.index) prev = entries[prev].next;
            entries[prev].next = e.next;
            if (m.hand == (int)e.index) m.hand = e.next;
        }
        e.next = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries) {
        if (m.hand == -1) return nullptr;
        int start = m.hand;
        do {
            if (!entries[m.hand].reference_bit) {
                Entry* victim = &entries[m.hand];
                m.hand = entries[m.hand].next;
                return victim;
            } else {
                entries[m.hand].reference_bit = false;
                m.hand = entries[m.hand].next;
            }
        } while (m.hand != start);
        return &entries[m.hand];
    }
    
    static const char* name() { return "CLOCK"; }
};

// Type aliases for common use cases
using LRUPolicy = LRU<uint64_t, uint64_t>;  // For sector-based caching
using LFUPolicy = LFU<uint64_t, uint64_t>;
using FIFOPolicy = FIFO<uint64_t, uint64_t>;
using CLOCKPolicy = CLOCK<uint64_t, uint64_t>;

#endif // POLICY_TRAITS_HPP 