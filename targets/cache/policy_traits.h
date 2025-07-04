// SPDX-License-Identifier: MIT or GPL-2.0-only

#ifndef POLICY_TRAITS_HPP
#define POLICY_TRAITS_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include <type_traits>
#include <functional>
#include <iostream>

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
    int pin_count = 0;
    
    CacheEntry() : key(), value(), dirty(false), valid(false), index(0), pin_count(0) {}
    CacheEntry(const Key& k, const Value& v, uint32_t idx)
        : key(k), value(v), dirty(false), valid(true), index(idx), pin_count(0) {}
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
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // LRU: walk from tail to head
        int idx = m.tail;
        while (idx != -1) {
            Entry& entry = entries[idx];
            if (can_evict(entry)) return &entry;
            idx = entry.prev;
        }
        return nullptr;
    }
    
    static const char* name() { return "LRU"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        // LRU doesn't have policy-specific stats beyond the basic ones
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  LRU list: ";
        int idx = m.head;
        while (idx != -1) {
            const Entry& entry = entries[idx];
            std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << ")";
            if (entry.next != -1) std::cout << " -> ";
            idx = entry.next;
        }
        std::cout << "\n";
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ""; // LRU doesn't have additional entry info
    }
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
        // Add to new bucket (at tail to preserve recently added entries)
        e.access_count++;
        e.prev_in_bucket = m.bucket_tails.count(e.access_count) ? m.bucket_tails[e.access_count] : -1;
        e.next_in_bucket = -1;
        if (e.prev_in_bucket != -1) entries[e.prev_in_bucket].next_in_bucket = e.index;
        m.bucket_tails[e.access_count] = e.index;
        if (!m.bucket_heads.count(e.access_count)) m.bucket_heads[e.access_count] = e.index;
        if (m.min_count > e.access_count || m.bucket_heads.empty()) m.min_count = e.access_count;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        e.access_count = 1;
        e.prev_in_bucket = m.bucket_tails.count(1) ? m.bucket_tails[1] : -1;
        e.next_in_bucket = -1;
        if (e.prev_in_bucket != -1) entries[e.prev_in_bucket].next_in_bucket = e.index;
        m.bucket_tails[1] = e.index;
        if (!m.bucket_heads.count(1)) m.bucket_heads[1] = e.index;
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
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // LFU: walk through min frequency bucket
        if (m.bucket_heads.empty()) return nullptr;
        int idx = m.bucket_heads[m.min_count];
        while (idx != -1) {
            Entry& entry = entries[idx];
            if (can_evict(entry)) return &entry;
            idx = entry.next_in_bucket;
        }
        return nullptr;
    }
    
    static const char* name() { return "LFU"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        // LFU doesn't have policy-specific stats beyond the basic ones
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  LFU frequency buckets:\n";
        for (const auto& bucket : m.bucket_heads) {
            uint64_t count = bucket.first;
            int head_idx = bucket.second;
            std::cout << "    frequency " << count << ": ";
            int idx = head_idx;
            while (idx != -1) {
                const Entry& entry = entries[idx];
                std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << ")";
                if (entry.next_in_bucket != -1) std::cout << " -> ";
                idx = entry.next_in_bucket;
            }
            std::cout << "\n";
        }
        std::cout << "  min_count: " << m.min_count << "\n";
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ", frequency: " + std::to_string(entry.access_count);
    }
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
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // FIFO: walk from head to tail
        int idx = m.head;
        while (idx != -1) {
            Entry& entry = entries[idx];
            if (can_evict(entry)) return &entry;
            idx = entry.next;
        }
        return nullptr;
    }
    
    static const char* name() { return "FIFO"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        // FIFO doesn't have policy-specific stats beyond the basic ones
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  FIFO list: ";
        int idx = m.head;
        while (idx != -1) {
            const Entry& entry = entries[idx];
            std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << ")";
            if (entry.next != -1) std::cout << " -> ";
            idx = entry.next;
        }
        std::cout << "\n";
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ""; // FIFO doesn't have additional entry info
    }
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
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // CLOCK: walk the clock
        if (m.hand == -1) return nullptr;
        int start = m.hand;
        do {
            Entry& entry = entries[m.hand];
            if (can_evict(entry)) {
                Entry* victim = &entry;
                m.hand = entry.next;
                return victim;
            }
            m.hand = entry.next;
        } while (m.hand != start);
        return nullptr;
    }
    
    static const char* name() { return "CLOCK"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        // CLOCK doesn't have policy-specific stats beyond the basic ones
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  CLOCK hand: " << (m.hand == -1 ? "none" : std::to_string(m.hand)) << "\n";
        if (m.hand != -1) {
            std::cout << "  CLOCK list: ";
            int start = m.hand;
            int idx = start;
            do {
                const Entry& entry = entries[idx];
                std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << "," << (entry.reference_bit ? "R" : "r") << ")";
                if (entry.next != idx) std::cout << " -> ";
                idx = entry.next;
            } while (idx != start);
            std::cout << "\n";
        }
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ", reference_bit: " + std::string(entry.reference_bit ? "true" : "false");
    }
};

// ===================== SIEVE Policy =====================
template<typename Key, typename Value>
class SIEVE {
public:
    struct Entry : public CacheEntry<Key, Value> {
        int next;
        bool visited;
        Entry() : CacheEntry<Key, Value>(), next(-1), visited(false) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), next(-1), visited(true) {}
    };
    
    struct ManagerData {
        int hand = -1;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData&, Entry& e, std::vector<Entry>&) {
        e.visited = true;
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
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        if (m.hand == -1) return nullptr;
        int start = m.hand;
        // First pass: clear visited bits
        do {
            Entry& entry = entries[m.hand];
            if (can_evict(entry)) {
                if (!entry.visited) {
                    Entry* victim = &entry;
                    m.hand = entry.next;
                    return victim;
                } else {
                    entry.visited = false;
                }
            }
            m.hand = entry.next;
        } while (m.hand != start);
        // Second pass: now all evictable entries are unvisited
        do {
            Entry& entry = entries[m.hand];
            if (can_evict(entry) && !entry.visited) {
                Entry* victim = &entry;
                m.hand = entry.next;
                return victim;
            }
            m.hand = entry.next;
        } while (m.hand != start);
        return nullptr;
    }
    
    static const char* name() { return "SIEVE"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        // SIEVE doesn't have policy-specific stats beyond the basic ones
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  SIEVE hand: " << (m.hand == -1 ? "none" : std::to_string(m.hand)) << "\n";
        if (m.hand != -1) {
            std::cout << "  SIEVE list: ";
            int start = m.hand;
            int idx = start;
            do {
                const Entry& entry = entries[idx];
                std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << "," << (entry.visited ? "V" : "v") << ")";
                if (entry.next != idx) std::cout << " -> ";
                idx = entry.next;
            } while (idx != start);
            std::cout << "\n";
        }
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ", visited: " + std::string(entry.visited ? "true" : "false");
    }
};

// Type aliases for common use cases
using LRUPolicy = LRU<uint64_t, uint64_t>;  // For sector-based caching
using LFUPolicy = LFU<uint64_t, uint64_t>;
using FIFOPolicy = FIFO<uint64_t, uint64_t>;
using CLOCKPolicy = CLOCK<uint64_t, uint64_t>;
using SIEVEPolicy = SIEVE<uint64_t, uint64_t>;

#endif // POLICY_TRAITS_HPP 