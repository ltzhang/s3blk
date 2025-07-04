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
#include <cassert>

// Forward declaration
template<typename Key, typename Value, template<typename, typename> class PolicyTemplate>
class TemplateCacheManager;

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

// Base policy interface - removed for now as we're keeping static functions

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
        int next, prev;
        bool reference_bit;
        Entry() : CacheEntry<Key, Value>(), next(-1), prev(-1), reference_bit(false) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), next(-1), prev(-1), reference_bit(true) {}
    };
    
    struct ManagerData {
        int hand = -1;
        int tail = -1;  // Add tail pointer for O(1) inserts
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData&, Entry& e, std::vector<Entry>&) {
        e.reference_bit = true;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (m.hand == -1) {
            // First entry
            m.hand = e.index;
            m.tail = e.index;
            e.next = e.index;
            e.prev = e.index;
        } else {
            // Insert after tail
            entries[m.tail].next = e.index;
            e.prev = m.tail;
            e.next = m.hand;
            entries[m.hand].prev = e.index;
            m.tail = e.index;
        }
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.next == e.index) {
            // Only one entry, remove it
            m.hand = -1;
            m.tail = -1;
        } else {
            // O(1) removal using prev pointer
            entries[e.prev].next = e.next;
            entries[e.next].prev = e.prev;
            // Update hand and tail pointers if needed
            if (m.hand == (int)e.index) m.hand = e.next;
            if (m.tail == (int)e.index) m.tail = e.prev;
            // If after removal, hand or tail points to an invalid entry, advance to next valid
            // (should not happen if only valid entries are in the list)
        }
        e.next = e.prev = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // CLOCK: walk the clock - up to two full passes
        if (m.hand == -1) return nullptr;
        int start = m.hand;
        int pass = 0;
        do {
            Entry& entry = entries[m.hand];
            if (can_evict(entry)) {
                if (!entry.reference_bit) {
                    // Found a victim with reference bit = false
                    Entry* victim = &entry;
                    m.hand = entry.next;
                    return victim;
                } else {
                    // Clear reference bit and give second chance
                    entry.reference_bit = false;
                }
            }
            m.hand = entry.next;
            if (m.hand == start) {
                pass++;
                if (pass == 2) break;
            }
        } while (true);
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
        int next, prev;
        bool visited;
        Entry() : CacheEntry<Key, Value>(), next(-1), prev(-1), visited(false) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), next(-1), prev(-1), visited(true) {}
    };
    
    struct ManagerData {
        int hand = -1;
        int tail = -1;  // Add tail pointer for O(1) inserts
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData&, Entry& e, std::vector<Entry>&) {
        e.visited = true;
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (m.hand == -1) {
            // First entry
            m.hand = e.index;
            m.tail = e.index;
            e.next = e.index;
            e.prev = e.index;
        } else {
            // Insert after tail for O(1) operation
            entries[m.tail].next = e.index;
            e.prev = m.tail;
            e.next = m.hand;
            entries[m.hand].prev = e.index;
            m.tail = e.index;
        }
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.next == e.index) {
            // Only one entry, remove it
            m.hand = -1;
            m.tail = -1;
        } else {
            // O(1) removal using prev pointer
            entries[e.prev].next = e.next;
            entries[e.next].prev = e.prev;
            
            // Update hand and tail pointers
            if (m.hand == (int)e.index) m.hand = e.next;
            if (m.tail == (int)e.index) m.tail = e.prev;
        }
        e.next = e.prev = -1;
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        // SIEVE: walk the sieve - up to two full passes
        if (m.hand == -1) return nullptr;
        int start = m.hand;
        int pass = 0;
        
        do {
            Entry& entry = entries[m.hand];
            if (can_evict(entry)) {
                if (!entry.visited) {
                    // Found a victim that hasn't been visited
                    Entry* victim = &entry;
                    m.hand = entry.next;
                    return victim;
                } else {
                    // Clear visited bit and give second chance
                    entry.visited = false;
                }
            }
            m.hand = entry.next;
            if (m.hand == start) {
                pass++;
                if (pass == 2) break;
            }
        } while (true);
        
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

// ===================== ARC Policy =====================
template<typename Key, typename Value>
class ARC {
public:
    struct Entry : public CacheEntry<Key, Value> {
        int prev, next;
        bool in_t1;
        Entry() : CacheEntry<Key, Value>(), prev(-1), next(-1), in_t1(true) {}
        Entry(const Key& k, const Value& v, uint32_t idx) 
            : CacheEntry<Key, Value>(k, v, idx), prev(-1), next(-1), in_t1(true) {}
    };
    
    struct ManagerData {
        // T1: recent entries (LRU), T2: frequent entries (LRU)
        int t1_head = -1, t1_tail = -1;
        int t2_head = -1, t2_tail = -1;
        
        // Ghost lists: B1 (recently evicted from T1), B2 (recently evicted from T2)
        std::unordered_map<Key, bool> b1_ghost;  // key -> true (ghost entry)
        std::unordered_map<Key, bool> b2_ghost;  // key -> true (ghost entry)
        
        // Adaptive parameter p (0 <= p <= c, where c is cache capacity)
        int p = 0;
        int capacity = 0;
    };
    
    using EntryType = Entry;
    using ManagerDataType = ManagerData;
    
    static void on_access(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.in_t1) {
            // Move from T1 to T2 (promotion)
            remove_from_list(m.t1_head, m.t1_tail, e, entries);
            add_to_list_head(m.t2_head, m.t2_tail, e, entries);
            e.in_t1 = false;
        } else {
            // Already in T2, move to head (LRU update)
            remove_from_list(m.t2_head, m.t2_tail, e, entries);
            add_to_list_head(m.t2_head, m.t2_tail, e, entries);
        }
    }
    
    static void on_insert(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        // Check if key exists in ghost lists
        bool in_b1 = m.b1_ghost.count(e.key) > 0;
        bool in_b2 = m.b2_ghost.count(e.key) > 0;
        
        if (in_b1) {
            // Case I: key in B1 (recently evicted from T1)
            // Increase p to favor T1
            m.p = std::min(m.p + std::max(1, (int)(m.b2_ghost.size() / m.b1_ghost.size())), m.capacity);
            m.b1_ghost.erase(e.key);
            // Insert into T2
            add_to_list_head(m.t2_head, m.t2_tail, e, entries);
            e.in_t1 = false;
        } else if (in_b2) {
            // Case II: key in B2 (recently evicted from T2)
            // Decrease p to favor T2
            m.p = std::max(m.p - std::max(1, (int)(m.b1_ghost.size() / m.b2_ghost.size())), 0);
            m.b2_ghost.erase(e.key);
            // Insert into T2
            add_to_list_head(m.t2_head, m.t2_tail, e, entries);
            e.in_t1 = false;
        } else {
            // Case III: key not in ghost lists
            // Insert into T1
            add_to_list_head(m.t1_head, m.t1_tail, e, entries);
            e.in_t1 = true;
        }
    }
    
    static void on_remove(ManagerData& m, Entry& e, std::vector<Entry>& entries) {
        if (e.in_t1) {
            remove_from_list(m.t1_head, m.t1_tail, e, entries);
            // Add to B1 ghost list
            m.b1_ghost[e.key] = true;
        } else {
            remove_from_list(m.t2_head, m.t2_tail, e, entries);
            // Add to B2 ghost list
            m.b2_ghost[e.key] = true;
        }
    }
    
    static Entry* get_eviction_candidate(ManagerData& m, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        int t1_size = count_list(m.t1_head, entries);
        int t2_size = count_list(m.t2_head, entries);
        
        // Case A: T1 has more than p elements
        if (t1_size > m.p) {
            // Evict from T1 tail
            return find_evictable_from_tail(m.t1_tail, entries, can_evict);
        }
        // Case B: T1 has exactly p elements and T2 is not empty
        else if (t1_size == m.p && t2_size > 0) {
            // Evict from T2 tail
            return find_evictable_from_tail(m.t2_tail, entries, can_evict);
        }
        // Case C: T1 has less than p elements
        else {
            // Evict from T1 tail
            return find_evictable_from_tail(m.t1_tail, entries, can_evict);
        }
    }
    
    static const char* name() { return "ARC"; }
    
    static void print_stats(const ManagerData& m, const std::vector<Entry>& entries) {
        int t1_size = count_list(m.t1_head, entries);
        int t2_size = count_list(m.t2_head, entries);
        std::cout << "  ARC T1 size: " << t1_size << ", T2 size: " << t2_size 
                  << ", B1=" << m.b1_ghost.size() << ", B2=" << m.b2_ghost.size() 
                  << ", p=" << m.p << "/" << m.capacity << "\n";
    }
    
    static void print_state(const ManagerData& m, const std::vector<Entry>& entries) {
        std::cout << "  ARC T1 (recent): ";
        print_list(m.t1_head, entries);
        std::cout << "  ARC T2 (frequent): ";
        print_list(m.t2_head, entries);
        std::cout << "  ARC B1 ghost: " << m.b1_ghost.size() << " entries\n";
        std::cout << "  ARC B2 ghost: " << m.b2_ghost.size() << " entries\n";
        std::cout << "  ARC p: " << m.p << "/" << m.capacity << "\n";
    }
    
    static std::string get_entry_info(const Entry& entry) {
        return ", in_t1: " + std::string(entry.in_t1 ? "true" : "false");
    }
    
private:
    static void add_to_list_head(int& head, int& tail, Entry& e, std::vector<Entry>& entries) {
        e.next = head;
        e.prev = -1;
        if (head != -1) entries[head].prev = e.index;
        head = e.index;
        if (tail == -1) tail = e.index;
    }
    
    static void remove_from_list(int& head, int& tail, Entry& e, std::vector<Entry>& entries) {
        if (e.prev != -1) entries[e.prev].next = e.next;
        if (e.next != -1) entries[e.next].prev = e.prev;
        if (head == (int)e.index) head = e.next;
        if (tail == (int)e.index) tail = e.prev;
        e.prev = e.next = -1;
    }
    
    static int count_list(int head, const std::vector<Entry>& entries) {
        int count = 0;
        int idx = head;
        while (idx != -1) {
            count++;
            idx = entries[idx].next;
        }
        return count;
    }
    
    static Entry* find_evictable_from_tail(int tail, std::vector<Entry>& entries, const std::function<bool(const Entry&)>& can_evict) {
        int idx = tail;
        while (idx != -1) {
            Entry& entry = entries[idx];
            if (can_evict(entry)) return &entry;
            idx = entry.prev;
        }
        return nullptr;
    }
    
    static void print_list(int head, const std::vector<Entry>& entries) {
        int idx = head;
        while (idx != -1) {
            const Entry& entry = entries[idx];
            std::cout << entry.key << "(" << entry.pin_count << "," << (entry.dirty ? "D" : "C") << ")";
            if (entry.next != -1) std::cout << " -> ";
            idx = entry.next;
        }
        std::cout << "\n";
    }
};

// Type aliases for common use cases
using LRUPolicy = LRU<uint64_t, uint64_t>;  // For sector-based caching
using LFUPolicy = LFU<uint64_t, uint64_t>;
using FIFOPolicy = FIFO<uint64_t, uint64_t>;
using CLOCKPolicy = CLOCK<uint64_t, uint64_t>;
using SIEVEPolicy = SIEVE<uint64_t, uint64_t>;
using ARCPolicy = ARC<uint64_t, uint64_t>;

#endif // POLICY_TRAITS_HPP 