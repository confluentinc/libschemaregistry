/**
 * TTL LRU Cache Implementation
 * Thread-safe cache with configurable capacity and TTL
 */

#pragma once

#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace schemaregistry::rest {

template <typename K, typename V>
class TtlLruCache {
  private:
    struct CacheEntry {
        V value;
        std::chrono::steady_clock::time_point timestamp;
        typename std::list<K>::iterator lru_iterator;

        CacheEntry(V val, std::chrono::steady_clock::time_point ts,
                   typename std::list<K>::iterator it)
            : value(std::move(val)), timestamp(ts), lru_iterator(it) {}
    };

    mutable std::mutex mutex_;
    std::unordered_map<K, CacheEntry> cache_;
    std::list<K>
        lru_list_;  // Most recently used at front, least recently used at back
    size_t capacity_;
    std::chrono::seconds ttl_;

    // Remove expired entries (must be called with mutex held)
    void cleanup_expired_unsafe() {
        auto now = std::chrono::steady_clock::now();
        auto it = cache_.begin();
        while (it != cache_.end()) {
            if (now - it->second.timestamp > ttl_) {
                // Remove from LRU list
                lru_list_.erase(it->second.lru_iterator);
                // Remove from cache
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Move key to front of LRU list (must be called with mutex held)
    void move_to_front_unsafe(const K& key) {
        auto& entry = cache_.at(key);
        // Remove from current position
        lru_list_.erase(entry.lru_iterator);
        // Add to front
        lru_list_.push_front(key);
        // Update iterator
        entry.lru_iterator = lru_list_.begin();
    }

    // Evict least recently used entry (must be called with mutex held)
    void evict_lru_unsafe() {
        if (!lru_list_.empty()) {
            const K& lru_key = lru_list_.back();
            cache_.erase(lru_key);
            lru_list_.pop_back();
        }
    }

  public:
    /**
     * Constructor
     * @param capacity Maximum number of entries to store
     * @param ttl Time-to-live for cache entries
     */
    TtlLruCache(size_t capacity, std::chrono::seconds ttl)
        : capacity_(capacity), ttl_(ttl) {
        if (capacity == 0) {
            throw std::invalid_argument(
                "Cache capacity must be greater than 0");
        }
    }

    /**
     * Get value from cache
     * @param key The key to lookup
     * @return Optional value if found and not expired, std::nullopt otherwise
     */
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Clean up expired entries periodically
        cleanup_expired_unsafe();

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }

        // Check if entry is expired
        auto now = std::chrono::steady_clock::now();
        if (now - it->second.timestamp > ttl_) {
            // Remove expired entry
            lru_list_.erase(it->second.lru_iterator);
            cache_.erase(it);
            return std::nullopt;
        }

        // Move to front of LRU list (mark as recently used)
        move_to_front_unsafe(key);

        // Update timestamp to extend TTL
        it->second.timestamp = now;

        return it->second.value;
    }

    /**
     * Put value into cache
     * @param key The key to store
     * @param value The value to store
     */
    void put(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();

        // Check if key already exists
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // Update existing entry
            it->second.value = value;
            it->second.timestamp = now;
            move_to_front_unsafe(key);
            return;
        }

        // Clean up expired entries before adding new ones
        cleanup_expired_unsafe();

        // Check capacity and evict if necessary
        while (cache_.size() >= capacity_) {
            evict_lru_unsafe();
        }

        // Add new entry
        lru_list_.push_front(key);
        cache_.emplace(key, CacheEntry(value, now, lru_list_.begin()));
    }

    /**
     * Clear all entries from cache
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_list_.clear();
    }

    /**
     * Get current cache size
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

    /**
     * Get cache capacity
     */
    size_t capacity() const { return capacity_; }

    /**
     * Get TTL setting
     */
    std::chrono::seconds ttl() const { return ttl_; }

    /**
     * Manually cleanup expired entries
     */
    void cleanup_expired() {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_unsafe();
    }
};

}  // namespace schemaregistry::rest
