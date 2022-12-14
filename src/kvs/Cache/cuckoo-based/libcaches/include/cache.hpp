#ifndef CACHE_HPP
#define CACHE_HPP

#include "cache_policy.hpp"

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace caches
{

// Base class for caching algorithms
template <typename Key, typename Value, typename Policy = NoCachePolicy<Key>>
class fixed_sized_cache
{
  public:
    using iterator = typename std::unordered_map<Key, Value>::iterator;
    using const_iterator =
        typename std::unordered_map<Key, Value>::const_iterator;
    using operation_guard = typename std::lock_guard<std::mutex>;
    using Callback =
        typename std::function<void(const Key &key, const Value &value)>;

    explicit fixed_sized_cache(
        size_t max_size,
        Callback OnErase = [](const Key &, const Value &) {},
        const Policy &policy = Policy())
        : cache_policy(policy), max_cache_size(max_size),
          OnEraseCallback(OnErase)
    {
        if (max_cache_size == 0)
        {
            max_cache_size = std::numeric_limits<size_t>::max();
        }
    }

    ~fixed_sized_cache()
    {
        Clear();
    }

    void Put(const Key &key, const Value &value)
    {
        //operation_guard lock{safe_op};
        auto elem_it = FindElem(key);

        if (elem_it == cache_items_map.end())
        {
            // add new element to the cache
            if (cache_items_map.size() + 1 > max_cache_size)
            {
                auto disp_candidate_key = cache_policy.ReplCandidate();

                Erase(disp_candidate_key);
            }

            Insert(key, value);
        }
        else
        {
            // update previous value
            Update(key, value);
        }
    }

    void weightPut(const Key &key, const Value &value, const uint64_t weight)
    {
        auto elem_it = FindElem(key);
        if (elem_it == cache_items_map.end())
        {
            weightInsert(key, value, weight);
        } else {
            weightUpdate(key, value, weight);
        }
    }

    const Value Get(const Key &key) const
    {
        //operation_guard lock{safe_op};
        auto elem_it = FindElem(key);

        if (elem_it == cache_items_map.end())
        {
            return nullptr;
        }
        cache_policy.Touch(key);

        return elem_it->second;
    }

    bool Cached(const Key &key) const
    {
        //operation_guard lock{safe_op};
        return FindElem(key) != cache_items_map.end();
    }

    size_t Size() const
    {
        //operation_guard lock{safe_op};

        return cache_items_map.size();
    }

    /**
     * Remove an element specified by key
     * @param key
     * @return
     * @retval true if an element specified by key was found and deleted
     * @retval false if an element is not present in a cache
     */
    const Value Remove(const Key &key)
    {
        //operation_guard{safe_op};
        if (cache_items_map.find(key) == cache_items_map.cend())
        {
            return nullptr;
        }

        auto elem_it = Erase(key);
        return elem_it->second;
    }

    const Value Evict()
    {
        auto disp_candidate_key = cache_policy.ReplCandidate();
        auto elem_it = Erase(disp_candidate_key);
        return elem_it->second;
    }

    size_t AggregatedMinHits(size_t num_elements)
    {
        return cache_policy.SumMinHits(num_elements);
    }

    size_t hitCounter(const Key &key)
    {
        //printf("[%s] Hits = %lu\n", __func__, cache_policy.Hits(key));
        return cache_policy.Hits(key);
    }

    void Clear()
    {
        //operation_guard lock{safe_op};

        for (auto it = begin(); it != end(); ++it)
        {
            cache_policy.Erase(it->first);
            OnEraseCallback(it->first, it->second);
        }
        cache_items_map.clear();
    }

  protected:
    typename std::unordered_map<Key, Value>::const_iterator begin() const
    {
        return cache_items_map.begin();
    }

    typename std::unordered_map<Key, Value>::const_iterator end() const
    {
        return cache_items_map.end();
    }

  protected:
    void Insert(const Key &key, const Value &value)
    {
        cache_policy.Insert(key);
        cache_items_map.emplace(std::make_pair(key, value));
    }

    void weightInsert(const Key &key, const Value &value, const uint64_t weight)
    {
        cache_policy.weightInsert(key, weight);
        cache_items_map.emplace(std::make_pair(key, value));
    }

    const_iterator Erase(const Key &key)
    {
        cache_policy.Erase(key);

        auto elem_it = FindElem(key);
        //OnEraseCallback(key, elem_it->second);
        cache_items_map.erase(elem_it);
        return elem_it;
    }

    void Update(const Key &key, const Value &value)
    {
        //cache_policy.Touch(key);
        cache_items_map[key] = value;
    }

    void weightUpdate(const Key &key, const Value &value, const uint64_t weight)
    {
        cache_policy.weightTouch(key, weight);
        cache_items_map[key] = value;
    }

    const_iterator FindElem(const Key &key) const
    {
        return cache_items_map.find(key);
    }

  private:
    std::unordered_map<Key, Value> cache_items_map;
    mutable Policy cache_policy;
    mutable std::mutex safe_op;
    size_t max_cache_size;
    Callback OnEraseCallback;
};
} // namespace caches

#endif // CACHE_HPP
