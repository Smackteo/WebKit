/*
 * Copyright (C) 2005-2024 Apple Inc. All rights reserved.
 * Copyright (C) 2008 David Levin <levin@chromium.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <string.h>
#include <type_traits>
#include <utility>
#include <wtf/AlignedStorage.h>
#include <wtf/Assertions.h>
#include <wtf/DebugHeap.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/ValueCheck.h>
#include <wtf/WeakRandomNumber.h>

// Configuration of WTF::HashTable.
//  - 75% load factor for small tables.
//  - 50% load factor for large tables.
//  - Use quadratic probing.
//  - Always use power-of-two hashtable size, which is also important to make quadratic probing work.

#define DUMP_HASHTABLE_STATS 0
#define DUMP_HASHTABLE_STATS_PER_TABLE 0

#if DUMP_HASHTABLE_STATS_PER_TABLE
#include <wtf/DataLog.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WTF {

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(HashTable);

// Enables internal WTF consistency checks that are invoked automatically. Non-WTF callers can call checkTableConsistency() even if internal checks are disabled.
#define CHECK_HASHTABLE_CONSISTENCY 0

#ifdef NDEBUG
#define CHECK_HASHTABLE_ITERATORS 0
#define CHECK_HASHTABLE_USE_AFTER_DESTRUCTION 0
#else
#define CHECK_HASHTABLE_ITERATORS 1
#define CHECK_HASHTABLE_USE_AFTER_DESTRUCTION 1
#endif

#if DUMP_HASHTABLE_STATS

    struct HashTableStats {
        // The following variables are all atomically incremented when modified.
        WTF_EXPORT_PRIVATE static std::atomic<unsigned> numAccesses;
        WTF_EXPORT_PRIVATE static std::atomic<unsigned> numRehashes;
        WTF_EXPORT_PRIVATE static std::atomic<unsigned> numRemoves;
        WTF_EXPORT_PRIVATE static std::atomic<unsigned> numReinserts;

        // The following variables are only modified in the recordCollisionAtCount method within a mutex.
        WTF_EXPORT_PRIVATE static unsigned maxCollisions;
        WTF_EXPORT_PRIVATE static unsigned numCollisions;
        WTF_EXPORT_PRIVATE static unsigned collisionGraph[4096];

        WTF_EXPORT_PRIVATE static void recordCollisionAtCount(unsigned count);
        WTF_EXPORT_PRIVATE static void dumpStats();
    };

#endif

    template<typename HashTable, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    class HashTableIterator;
    template<typename HashTable, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    class HashTableConstIterator;

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    void addIterator(const HashTableType*, HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>*);

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    void removeIterator(HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>*);

    template<typename HashTableType>
    void invalidateIterators(const HashTableType*);

#if !CHECK_HASHTABLE_ITERATORS

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    inline void addIterator(const HashTableType*, HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>*) { }

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    inline void removeIterator(HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>*) { }

    template<typename HashTableType>
    void invalidateIterators(const HashTableType*) { }

#endif

    typedef enum { HashItemKnownGood } HashItemKnownGoodTag;

    template<typename HashTable, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    class HashTableConstIterator {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HashTableConstIterator);
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Value;
        using difference_type = ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

    private:
        using HashTableType = HashTable;
        typedef HashTableIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> iterator;
        typedef HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> const_iterator;
        typedef Value ValueType;
        typedef const ValueType& ReferenceType;
        typedef const ValueType* PointerType;

        friend HashTableType;
        friend iterator;

        void skipEmptyBuckets()
        {
            while (m_position != m_endPosition && HashTableType::isEmptyOrDeletedBucket(*m_position))
                ++m_position;
        }

        HashTableConstIterator(const HashTableType* table, PointerType position, PointerType endPosition)
            : m_position(position), m_endPosition(endPosition)
        {
            addIterator(table, this);
            skipEmptyBuckets();
        }

        HashTableConstIterator(const HashTableType* table, PointerType position, PointerType endPosition, HashItemKnownGoodTag)
            : m_position(position), m_endPosition(endPosition)
        {
            addIterator(table, this);
        }

    public:
        HashTableConstIterator()
        {
            addIterator(static_cast<const HashTableType*>(0), this);
        }

        // default copy, assignment and destructor are OK if CHECK_HASHTABLE_ITERATORS is 0

#if CHECK_HASHTABLE_ITERATORS
        ~HashTableConstIterator()
        {
            removeIterator(this);
        }

        HashTableConstIterator(const const_iterator& other)
            : m_position(other.m_position), m_endPosition(other.m_endPosition)
        {
            addIterator(other.m_table, this);
        }

        const_iterator& operator=(const const_iterator& other)
        {
            m_position = other.m_position;
            m_endPosition = other.m_endPosition;

            removeIterator(this);
            addIterator(other.m_table, this);

            return *this;
        }
#endif

        PointerType get() const
        {
            checkValidity();
            return m_position;
        }
        ReferenceType operator*() const { return *get(); }
        PointerType operator->() const { return get(); }

        const_iterator& operator++()
        {
            checkValidity();
            ASSERT(m_position != m_endPosition);
            ++m_position;
            skipEmptyBuckets();
            return *this;
        }

        // postfix ++ intentionally omitted

        // Comparison.
        bool operator==(const const_iterator& other) const
        {
            checkValidity(other);
            return m_position == other.m_position;
        }
        bool operator==(const iterator& other) const
        {
            return *this == static_cast<const_iterator>(other);
        }

    private:
        void checkValidity() const
        {
#if CHECK_HASHTABLE_ITERATORS
            ASSERT(m_table);
#endif
        }


#if CHECK_HASHTABLE_ITERATORS
        void checkValidity(const const_iterator& other) const
        {
            ASSERT(m_table);
            ASSERT_UNUSED(other, other.m_table);
            ASSERT(m_table == other.m_table);
        }
#else
        void checkValidity(const const_iterator&) const { }
#endif

        PointerType m_position { nullptr };
        PointerType m_endPosition { nullptr };

#if CHECK_HASHTABLE_ITERATORS
    public:
        // Any modifications of the m_next or m_previous of an iterator that is in a linked list of a HashTable::m_iterator,
        // should be guarded with m_table->m_mutex.
        mutable const HashTableType* m_table;
        mutable const_iterator* m_next;
        mutable const_iterator* m_previous;
#endif
    };

    template<typename HashTable, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    class HashTableIterator {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(HashTableIterator);
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Value;
        using difference_type = ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;

    private:
        using HashTableType = HashTable;
        typedef HashTableIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> iterator;
        typedef HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> const_iterator;
        typedef Value ValueType;
        typedef ValueType& ReferenceType;
        typedef ValueType* PointerType;

        friend HashTableType;

        HashTableIterator(HashTableType* table, PointerType pos, PointerType end) : m_iterator(table, pos, end) { }
        HashTableIterator(HashTableType* table, PointerType pos, PointerType end, HashItemKnownGoodTag tag) : m_iterator(table, pos, end, tag) { }

    public:
        HashTableIterator() { }

        // default copy, assignment and destructor are OK

        PointerType get() const { return const_cast<PointerType>(m_iterator.get()); }
        ReferenceType operator*() const { return *get(); }
        PointerType operator->() const { return get(); }

        iterator& operator++() { ++m_iterator; return *this; }

        // postfix ++ intentionally omitted

        // Comparison.
        bool operator==(const iterator& other) const { return m_iterator == other.m_iterator; }
        bool operator==(const const_iterator& other) const { return m_iterator == other; }

        operator const_iterator() const { return m_iterator; }

    private:
        const_iterator m_iterator;
    };

    template<typename ValueTraits, typename HashFunctions>
    class IdentityHashTranslator {
    public:
        static unsigned hash(const auto& key) { return HashFunctions::hash(key); }
        static bool equal(const auto& a, const auto& b) { return HashFunctions::equal(a, b); }
        static void translate(auto& location, const auto&, NOESCAPE const Invocable<typename ValueTraits::TraitType()> auto& functor)
        {
            ValueTraits::assignToEmpty(location, functor());
        }
    };

    template<typename IteratorType> struct HashTableAddResult {
        HashTableAddResult() : isNewEntry(false) { }
        HashTableAddResult(IteratorType iter, bool isNewEntry) : iterator(iter), isNewEntry(isNewEntry) { }
        IteratorType iterator;
        bool isNewEntry;

        explicit operator bool() const { return isNewEntry; }
    };

    // HashTableCapacityForSize computes the upper power of two capacity to hold the size parameter.
    // This is done at compile time to initialize the HashTraits.
    template<unsigned size>
    struct HashTableCapacityForSize {
        // Load-factor for small table is 75%.
        static constexpr unsigned smallMaxLoadNumerator = 3;
        static constexpr unsigned smallMaxLoadDenominator = 4;
        // Load-factor for large table is 50%.
        static constexpr unsigned largeMaxLoadNumerator = 1;
        static constexpr unsigned largeMaxLoadDenominator = 2;
        static constexpr unsigned maxSmallTableCapacity = 1024;
        static constexpr unsigned minLoad = 6;

        static constexpr bool shouldExpand(uint64_t keyAndDeleteCount, uint64_t tableSize)
        {
            if (tableSize <= maxSmallTableCapacity)
                return keyAndDeleteCount * smallMaxLoadDenominator >= tableSize * smallMaxLoadNumerator;
            return keyAndDeleteCount * largeMaxLoadDenominator >= tableSize * largeMaxLoadNumerator;
        }

        static constexpr unsigned capacityForSize(uint32_t sizeArg)
        {
            if (!sizeArg)
                return 0;
            constexpr unsigned maxCapacity = 1U << 31;
            UNUSED_PARAM(maxCapacity);
            ASSERT_UNDER_CONSTEXPR_CONTEXT(sizeArg <= maxCapacity);
            uint32_t capacity = roundUpToPowerOfTwo(sizeArg);
            ASSERT_UNDER_CONSTEXPR_CONTEXT(capacity <= maxCapacity);
            if (shouldExpand(sizeArg, capacity)) {
                ASSERT_UNDER_CONSTEXPR_CONTEXT((static_cast<uint64_t>(capacity) * 2) <= maxCapacity);
                return capacity * 2;
            }
            return capacity;
        }

        static constexpr unsigned value = capacityForSize(size);
        static_assert(size > 0, "HashTableNonZeroMinimumCapacity");
        static_assert(!static_cast<unsigned>(value >> 31), "HashTableNoCapacityOverflow");
    };

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    static inline void checkHashTableKey(const T& key)
    {
        if constexpr (!ASSERT_ENABLED && shouldValidateKey == ShouldValidateKey::No)
            return;

        if constexpr (std::is_convertible_v<T, Key>) {
            RELEASE_ASSERT(!isHashTraitsEmptyValue<KeyTraits>(key));
            RELEASE_ASSERT(!KeyTraits::isDeletedValue(key));
        } else if constexpr (HashFunctions::safeToCompareToEmptyOrDeleted) {
            RELEASE_ASSERT(!HashTranslator::equal(KeyTraits::emptyValue(), key));

            AlignedStorage<Value> deletedValueBuffer;
            auto& deletedValue = *deletedValueBuffer;
            Traits::constructDeletedValue(deletedValue);
            RELEASE_ASSERT(!HashTranslator::equal(Extractor::extract(deletedValue), key));
        }
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    class HashTable {
    public:
        using HashTableType = HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>;
        typedef HashTableIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> iterator;
        typedef HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits> const_iterator;
        typedef Traits ValueTraits;
        typedef Key KeyType;
        typedef Value ValueType;
        using TakeType = typename ValueTraits::TakeType;
        typedef IdentityHashTranslator<ValueTraits, HashFunctions> IdentityTranslatorType;
        typedef HashTableAddResult<iterator> AddResult;

        using HashTableSizePolicy = HashTableCapacityForSize<1>;

#if DUMP_HASHTABLE_STATS_PER_TABLE
        struct Stats {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Stats);

            Stats()
                : numAccesses(0)
                , numRehashes(0)
                , numRemoves(0)
                , numReinserts(0)
                , maxCollisions(0)
                , numCollisions(0)
                , collisionGraph()
            {
            }

            unsigned numAccesses;
            unsigned numRehashes;
            unsigned numRemoves;
            unsigned numReinserts;

            unsigned maxCollisions;
            unsigned numCollisions;
            unsigned collisionGraph[4096];

            void recordCollisionAtCount(unsigned count)
            {
                if (count > maxCollisions)
                    maxCollisions = count;
                numCollisions++;
                collisionGraph[count]++;
            }

            void dumpStats()
            {
                dataLogF("\nWTF::HashTable::Stats dump\n\n");
                dataLogF("%d accesses\n", numAccesses);
                dataLogF("%d total collisions, average %.2f probes per access\n", numCollisions, 1.0 * (numAccesses + numCollisions) / numAccesses);
                dataLogF("longest collision chain: %d\n", maxCollisions);
                for (unsigned i = 1; i <= maxCollisions; i++) {
                    dataLogF("  %d lookups with exactly %d collisions (%.2f%% , %.2f%% with this many or more)\n", collisionGraph[i], i, 100.0 * (collisionGraph[i] - collisionGraph[i+1]) / numAccesses, 100.0 * collisionGraph[i] / numAccesses);
                }
                dataLogF("%d rehashes\n", numRehashes);
                dataLogF("%d reinserts\n", numReinserts);
            }
        };
#endif

        HashTable();
        ~HashTable() 
        {
            invalidateIterators(this); 
            if (m_table)
                deallocateTable(m_table);
#if CHECK_HASHTABLE_USE_AFTER_DESTRUCTION
            m_table = (ValueType*)(uintptr_t)0xbbadbeef;
#endif
        }

        HashTable(const HashTable&);
        void swap(HashTable&);
        HashTable& operator=(const HashTable&);

        HashTable(HashTable&&);
        HashTable& operator=(HashTable&&);

        // When the hash table is empty, just return the same iterator for end as for begin.
        // This is more efficient because we don't have to skip all the empty and deleted
        // buckets, and iterating an empty table is a common case that's worth optimizing.
        iterator begin() LIFETIME_BOUND { return isEmpty() ? end() : makeIterator(m_table); }
        iterator end() LIFETIME_BOUND { return makeKnownGoodIterator(m_table + tableSize()); }
        const_iterator begin() const LIFETIME_BOUND { return isEmpty() ? end() : makeConstIterator(m_table); }
        const_iterator end() const LIFETIME_BOUND { return makeKnownGoodConstIterator(m_table + tableSize()); }

        iterator random() LIFETIME_BOUND
        {
            if (isEmpty())
                return end();

            while (true) {
                auto& bucket = m_table[weakRandomNumber<uint32_t>() & tableSizeMask()];
                if (!isEmptyOrDeletedBucket(bucket))
                    return makeKnownGoodIterator(&bucket);
            };
        }

        const_iterator random() const LIFETIME_BOUND { return static_cast<const_iterator>(const_cast<HashTable*>(this)->random()); }

        unsigned size() const { return keyCount(); }
        unsigned capacity() const { return tableSize(); }
        size_t byteSize() const { return metadataSize + tableSize() * sizeof(ValueType); }
        bool isEmpty() const { return !keyCount(); }

        void reserveInitialCapacity(unsigned keyCount)
        {
            ASSERT(!m_table);
            ASSERT(!tableSize());

            unsigned newTableSize = computeBestTableSize(keyCount);

            m_table = allocateTable(newTableSize);
            setTableSize(newTableSize);
            setTableSizeMask(newTableSize - 1);
            setDeletedCount(0);
            setKeyCount(0);
        }

        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> AddResult add(const ValueType& value) { return add<IdentityTranslatorType, shouldValidateKey>(Extractor::extract(value), [&]() ALWAYS_INLINE_LAMBDA { return value; }); }
        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> AddResult add(ValueType&& value) { return add<IdentityTranslatorType, shouldValidateKey>(Extractor::extract(value), [&]() ALWAYS_INLINE_LAMBDA { return WTFMove(value); }); }

        // A special version of add() that finds the object by hashing and comparing
        // with some other type, to avoid the cost of type conversion if the object is already
        // in the table.
        template<typename HashTranslator, ShouldValidateKey> AddResult add(auto&& key, NOESCAPE const std::invocable<> auto& functor);
        template<typename HashTranslator, ShouldValidateKey> AddResult addPassingHashCode(auto&& key, NOESCAPE const std::invocable<> auto& functor);

        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> iterator find(const KeyType& key) { return find<IdentityTranslatorType, shouldValidateKey>(key); }
        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> const_iterator find(const KeyType& key) const { return find<IdentityTranslatorType, shouldValidateKey>(key); }
        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> bool contains(const KeyType& key) const { return contains<IdentityTranslatorType, shouldValidateKey>(key); }

        template<typename HashTranslator, ShouldValidateKey, typename T> iterator find(const T&);
        template<typename HashTranslator, ShouldValidateKey, typename T> const_iterator find(const T&) const;
        template<typename HashTranslator, ShouldValidateKey, typename T> bool contains(const T&) const;

        template<ShouldValidateKey> void remove(const KeyType&);
        void remove(iterator);
        void removeWithoutEntryConsistencyCheck(iterator);
        void removeWithoutEntryConsistencyCheck(const_iterator);
        // FIXME: This feels like it should be Invocable<bool(const ValueType&)> but that breaks many HashMap users.
        bool removeIf(NOESCAPE const Invocable<bool(ValueType&)> auto&);
        void clear();

        template<size_t inlineCapacity>
        Vector<TakeType, inlineCapacity> takeIf(NOESCAPE const Invocable<bool(const ValueType&)> auto&);

        static bool isEmptyBucket(const ValueType& value) { return isHashTraitsEmptyValue<KeyTraits>(Extractor::extract(value)); }
        static bool isReleasedWeakBucket(const ValueType& value) { return isHashTraitsReleasedWeakValue<KeyTraits>(Extractor::extract(value)); }
        static bool isDeletedBucket(const ValueType& value) { return KeyTraits::isDeletedValue(Extractor::extract(value)); }
        static bool isEmptyOrDeletedBucket(const ValueType& value) { return isEmptyBucket(value) || isDeletedBucket(value); }

        template<ShouldValidateKey shouldValidateKey = ShouldValidateKey::Yes> ValueType* lookup(const Key& key) { return lookup<IdentityTranslatorType, shouldValidateKey>(key); }
        template<typename HashTranslator, ShouldValidateKey = ShouldValidateKey::Yes, typename T> ValueType* lookup(const T&);
        template<typename HashTranslator, ShouldValidateKey, typename T> ValueType* inlineLookup(const T&);

        ALWAYS_INLINE bool isNullStorage() const { return !m_table; }

#if ASSERT_ENABLED
        void checkTableConsistency() const;
#else
        static void checkTableConsistency() { }
#endif
#if CHECK_HASHTABLE_CONSISTENCY
        void internalCheckTableConsistency() const { checkTableConsistency(); }
        void internalCheckTableConsistencyExceptSize() const { checkTableConsistencyExceptSize(); }
#else
        static void internalCheckTableConsistencyExceptSize() { }
        static void internalCheckTableConsistency() { }
#endif

    private:
        static ValueType* allocateTable(unsigned size);
        static void deallocateTable(ValueType* table);

        typedef std::pair<ValueType*, bool> LookupType;
        typedef std::pair<LookupType, unsigned> FullLookupType;

        ValueType* lookupForReinsert(const Key& key) { return lookupForReinsert<IdentityTranslatorType>(key); };
        template<typename HashTranslator, typename T> ValueType* lookupForReinsert(const T&);
        template<typename HashTranslator, typename T> FullLookupType fullLookupForWriting(const T&);

        template<typename HashTranslator> void addUniqueForInitialization(auto&& key, NOESCAPE const std::invocable<> auto& functor);

        void removeAndInvalidateWithoutEntryConsistencyCheck(ValueType*);
        void removeAndInvalidate(ValueType*);
        void remove(ValueType*);

        static constexpr unsigned computeBestTableSize(unsigned keyCount);
        bool shouldExpand() const { return HashTableSizePolicy::shouldExpand(keyCount() + deletedCount(), tableSize()); }
        bool mustRehashInPlace() const { return keyCount() * minLoad < tableSize() * 2; }
        bool shouldShrink() const { return keyCount() * minLoad < tableSize() && tableSize() > KeyTraits::minimumTableSize; }
        ValueType* expand(ValueType* entry = nullptr);
        void shrink() { rehash(tableSize() / 2, nullptr); }
        void shrinkToBestSize();
    
        void deleteReleasedWeakBuckets();

        ValueType* rehash(unsigned newTableSize, ValueType* entry);
        ValueType* reinsert(ValueType&&);

        static void initializeBucket(ValueType& bucket);
        static void deleteBucket(ValueType& bucket) { hashTraitsDeleteBucket<Traits>(bucket); }

        FullLookupType makeLookupResult(ValueType* position, bool found, unsigned hash)
            { return FullLookupType(LookupType(position, found), hash); }

        iterator makeIterator(ValueType* pos) { return iterator(this, pos, m_table + tableSize()); }
        const_iterator makeConstIterator(ValueType* pos) const { return const_iterator(this, pos, m_table + tableSize()); }
        iterator makeKnownGoodIterator(ValueType* pos) { return iterator(this, pos, m_table + tableSize(), HashItemKnownGood); }
        const_iterator makeKnownGoodConstIterator(ValueType* pos) const { return const_iterator(this, pos, m_table + tableSize(), HashItemKnownGood); }

#if ASSERT_ENABLED
        void checkTableConsistencyExceptSize() const;
#else
        static void checkTableConsistencyExceptSize() { }
#endif

        // Load-factor for small table is 75%.
        static constexpr unsigned smallMaxLoadNumerator = HashTableSizePolicy::smallMaxLoadNumerator;
        static constexpr unsigned smallMaxLoadDenominator = HashTableSizePolicy::smallMaxLoadDenominator;
        // Load-factor for large table is 50%.
        static constexpr unsigned largeMaxLoadNumerator = HashTableSizePolicy::largeMaxLoadNumerator;
        static constexpr unsigned largeMaxLoadDenominator = HashTableSizePolicy::largeMaxLoadDenominator;
        static constexpr unsigned maxSmallTableCapacity = HashTableSizePolicy::maxSmallTableCapacity;
        static constexpr unsigned minLoad = HashTableSizePolicy::minLoad;

        static constexpr int tableSizeOffset = -1;
        static constexpr int tableSizeMaskOffset = -2;
        static constexpr int keyCountOffset = -3;
        static constexpr int deletedCountOffset = -4;
        static constexpr unsigned metadataSize = std::max(4 * sizeof(unsigned), alignof(ValueType));

        unsigned tableSize() const { return m_table ? reinterpret_cast_ptr<unsigned*>(m_table)[tableSizeOffset] : 0; }
        void setTableSize(unsigned size) const { ASSERT(m_table); reinterpret_cast_ptr<unsigned*>(m_table)[tableSizeOffset] = size; }
        unsigned tableSizeMask() const { ASSERT(m_table); return m_table ? reinterpret_cast_ptr<unsigned*>(m_table)[tableSizeMaskOffset] : 0; }
        void setTableSizeMask(unsigned mask) { ASSERT(m_table); reinterpret_cast_ptr<unsigned*>(m_table)[tableSizeMaskOffset] = mask; }
        unsigned keyCount() const { return m_table ? reinterpret_cast_ptr<unsigned*>(m_table)[keyCountOffset] : 0; }
        void setKeyCount(unsigned count) const { ASSERT(m_table); reinterpret_cast_ptr<unsigned*>(m_table)[keyCountOffset] = count; }
        unsigned deletedCount() const { ASSERT(m_table); return reinterpret_cast_ptr<unsigned*>(m_table)[deletedCountOffset]; }
        void setDeletedCount(unsigned count) const { ASSERT(m_table); reinterpret_cast_ptr<unsigned*>(m_table)[deletedCountOffset] = count; }

        ValueType* m_table { nullptr };

#if CHECK_HASHTABLE_ITERATORS
    public:
        // All access to m_iterators should be guarded with m_mutex.
        mutable const_iterator* m_iterators;
        // Use std::unique_ptr so HashTable can still be memmove'd or memcpy'ed.
        mutable std::unique_ptr<Lock> m_mutex;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
    public:
        mutable std::unique_ptr<Stats> m_stats;
#endif
    };

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::HashTable()
        : m_table(nullptr)
#if CHECK_HASHTABLE_ITERATORS
        , m_iterators(0)
        , m_mutex(makeUnique<Lock>())
#endif
#if DUMP_HASHTABLE_STATS_PER_TABLE
        , m_stats(makeUnique<Stats>())
#endif
    {
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::lookup(const T& key) -> ValueType*
    {
        return inlineLookup<HashTranslator, shouldValidateKey>(key);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    ALWAYS_INLINE auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::inlineLookup(const T& key) -> ValueType*
    {
        static_assert(sizeof(Value) <= 150, "Your HashTable types are too big to efficiently move when rehashing.  Consider using UniqueRef instead");
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, shouldValidateKey>(key);

        ValueType* table = m_table;
        if (!table)
            return nullptr;

        unsigned sizeMask = tableSizeMask();
        unsigned h = HashTranslator::hash(key);
        unsigned i = h & sizeMask;
        unsigned probeCount = 0;

#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numAccesses;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numAccesses;
#endif

        while (true) {
            ValueType* entry = table + i;
                
            // We count on the compiler to optimize out this branch.
            if constexpr (HashFunctions::safeToCompareToEmptyOrDeleted) {
                if (HashTranslator::equal(Extractor::extract(*entry), key))
                    return entry;
                
                if (isEmptyBucket(*entry))
                    return nullptr;
            } else {
                if (isEmptyBucket(*entry))
                    return nullptr;
                
                if (!isDeletedBucket(*entry) && HashTranslator::equal(Extractor::extract(*entry), key))
                    return entry;
            }

            ++probeCount;

#if DUMP_HASHTABLE_STATS
            HashTableStats::recordCollisionAtCount(probeCount);
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
            m_stats->recordCollisionAtCount(probeCount);
#endif

            i = (i + probeCount) & sizeMask;
        }
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, typename T>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::lookupForReinsert(const T& key) -> ValueType*
    {
        ASSERT(m_table);
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, ShouldValidateKey::No>(key);

        ValueType* table = m_table;
        unsigned sizeMask = tableSizeMask();
        unsigned h = HashTranslator::hash(key);
        unsigned i = h & sizeMask;
        unsigned probeCount = 0;

#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numAccesses;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numAccesses;
#endif

        while (true) {
            ValueType* entry = table + i;

            if (isEmptyBucket(*entry))
                return entry;

            ++probeCount;

#if DUMP_HASHTABLE_STATS
            HashTableStats::recordCollisionAtCount(probeCount);
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
            m_stats->recordCollisionAtCount(probeCount);
#endif

            i = (i + probeCount) & sizeMask;
        }
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, typename T>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::fullLookupForWriting(const T& key) -> FullLookupType
    {
        ASSERT(m_table);
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, ShouldValidateKey::No>(key);

        ValueType* table = m_table;
        unsigned sizeMask = tableSizeMask();
        unsigned h = HashTranslator::hash(key);
        unsigned i = h & sizeMask;
        unsigned probeCount = 0;

#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numAccesses;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numAccesses;
#endif

        ValueType* deletedEntry = nullptr;

        while (true) {
            ValueType* entry = table + i;
            
            // We count on the compiler to optimize out this branch.
            if constexpr (HashFunctions::safeToCompareToEmptyOrDeleted) {
                if (isEmptyBucket(*entry))
                    return makeLookupResult(deletedEntry ? deletedEntry : entry, false, h);
                
                if (HashTranslator::equal(Extractor::extract(*entry), key))
                    return makeLookupResult(entry, true, h);
                
                if (isDeletedBucket(*entry))
                    deletedEntry = entry;
            } else {
                if (isEmptyBucket(*entry))
                    return makeLookupResult(deletedEntry ? deletedEntry : entry, false, h);
            
                if (isDeletedBucket(*entry))
                    deletedEntry = entry;
                else if (HashTranslator::equal(Extractor::extract(*entry), key))
                    return makeLookupResult(entry, true, h);
            }

            ++probeCount;

#if DUMP_HASHTABLE_STATS
            HashTableStats::recordCollisionAtCount(probeCount);
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
            m_stats->recordCollisionAtCount(probeCount);
#endif

            i = (i + probeCount) & sizeMask;
        }
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, typename T>
    ALWAYS_INLINE void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::addUniqueForInitialization(T&& key, NOESCAPE const std::invocable<> auto& functor)
    {
        ASSERT(m_table);
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, ShouldValidateKey::No>(key);
        invalidateIterators(this);

        internalCheckTableConsistency();

        Value* entry = lookupForReinsert<HashTranslator>(key);
        HashTranslator::translate(*entry, std::forward<T>(key), functor);

        internalCheckTableConsistency();
    }

    template<typename Traits, typename Value>
    static void initializeHashTableBucket(Value& bucket)
    {
        if constexpr (Traits::emptyValueIsZero) {
            // This initializes the bucket without copying the empty value.
            // That makes it possible to use this with types that don't support copying.
            // The memset to 0 looks like a slow operation but is optimized by the compilers.
            zeroBytes(bucket);
        } else
            Traits::template constructEmptyValue<Traits>(bucket);
    }
    
    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::initializeBucket(ValueType& bucket)
    {
        initializeHashTableBucket<Traits>(bucket);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    ALWAYS_INLINE auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::add(T&& key, NOESCAPE const std::invocable<> auto& functor) -> AddResult
    {
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, shouldValidateKey>(key);
        invalidateIterators(this);

        if (!m_table)
            expand(nullptr);

        internalCheckTableConsistency();

        ASSERT(m_table);

        ValueType* table = m_table;
        unsigned sizeMask = tableSizeMask();
        unsigned h = HashTranslator::hash(key);
        unsigned i = h & sizeMask;
        unsigned probeCount = 0;

#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numAccesses;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numAccesses;
#endif

        ValueType* deletedEntry = nullptr;
        ValueType* entry;
        while (true) {
            entry = table + i;
            
            // We count on the compiler to optimize out this branch.
            if constexpr (HashFunctions::safeToCompareToEmptyOrDeleted) {
                if (isEmptyBucket(*entry))
                    break;
                
                if (HashTranslator::equal(Extractor::extract(*entry), key))
                    return AddResult(makeKnownGoodIterator(entry), false);
                
                if (isDeletedBucket(*entry))
                    deletedEntry = entry;
            } else {
                if (isEmptyBucket(*entry))
                    break;
            
                if (isDeletedBucket(*entry))
                    deletedEntry = entry;
                else if (HashTranslator::equal(Extractor::extract(*entry), key))
                    return AddResult(makeKnownGoodIterator(entry), false);
            }

            ++probeCount;

#if DUMP_HASHTABLE_STATS
            HashTableStats::recordCollisionAtCount(probeCount);
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
            m_stats->recordCollisionAtCount(probeCount);
#endif

            i = (i + probeCount) & sizeMask;
        }

        if (deletedEntry) {
            initializeBucket(*deletedEntry);
            entry = deletedEntry;
            setDeletedCount(deletedCount() - 1);
        }

        HashTranslator::translate(*entry, std::forward<T>(key), functor);
        setKeyCount(keyCount() + 1);
        
        if (shouldExpand())
            entry = expand(entry);

        internalCheckTableConsistency();
        
        return AddResult(makeKnownGoodIterator(entry), true);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::addPassingHashCode(T&& key, NOESCAPE const std::invocable<> auto& functor) -> AddResult
    {
        checkHashTableKey<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, HashTranslator, shouldValidateKey>(key);

        invalidateIterators(this);

        if (!m_table)
            expand();

        internalCheckTableConsistency();

        FullLookupType lookupResult = fullLookupForWriting<HashTranslator>(key);

        ValueType* entry = lookupResult.first.first;
        bool found = lookupResult.first.second;
        unsigned h = lookupResult.second;
        
        if (found)
            return AddResult(makeKnownGoodIterator(entry), false);
        
        if (isDeletedBucket(*entry)) {
            initializeBucket(*entry);
            setDeletedCount(deletedCount() - 1);
        }

        HashTranslator::translate(*entry, std::forward<T>(key), functor, h);
        setKeyCount(keyCount() + 1);

        if (shouldExpand())
            entry = expand(entry);

        internalCheckTableConsistency();

        return AddResult(makeKnownGoodIterator(entry), true);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::reinsert(ValueType&& entry) -> ValueType*
    {
        ASSERT(m_table);
        ASSERT(!isDeletedBucket(*lookupForReinsert(Extractor::extract(entry))));
        ASSERT(isEmptyBucket(*lookupForReinsert(Extractor::extract(entry))));
#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numReinserts;
#endif
#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numReinserts;
#endif

        Value* newEntry = lookupForReinsert(Extractor::extract(entry));
        newEntry->~Value();
        new (NotNull, newEntry) ValueType(WTFMove(entry));

        return newEntry;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template <typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::find(const T& key) -> iterator
    {
        if (!m_table)
            return end();

        ValueType* entry = lookup<HashTranslator, shouldValidateKey>(key);
        if (!entry)
            return end();

        return makeKnownGoodIterator(entry);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template <typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::find(const T& key) const -> const_iterator
    {
        if (!m_table)
            return end();

        ValueType* entry = const_cast<HashTable*>(this)->lookup<HashTranslator, shouldValidateKey>(key);
        if (!entry)
            return end();

        return makeKnownGoodConstIterator(entry);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template <typename HashTranslator, ShouldValidateKey shouldValidateKey, typename T>
    bool HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::contains(const T& key) const
    {
        if (!m_table)
            return false;

        return const_cast<HashTable*>(this)->lookup<HashTranslator, shouldValidateKey>(key);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::removeAndInvalidateWithoutEntryConsistencyCheck(ValueType* pos)
    {
        invalidateIterators(this);
        remove(pos);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::removeAndInvalidate(ValueType* pos)
    {
        invalidateIterators(this);
        internalCheckTableConsistency();
        remove(pos);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::remove(ValueType* pos)
    {
#if DUMP_HASHTABLE_STATS
        ++HashTableStats::numRemoves;
#endif
#if DUMP_HASHTABLE_STATS_PER_TABLE
        ++m_stats->numRemoves;
#endif

        deleteBucket(*pos);
        setDeletedCount(deletedCount() + 1);
        setKeyCount(keyCount() - 1);

        if (shouldShrink())
            shrink();

        internalCheckTableConsistency();
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::remove(iterator it)
    {
        if (it == end())
            return;

        removeAndInvalidate(const_cast<ValueType*>(it.m_iterator.m_position));
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::removeWithoutEntryConsistencyCheck(iterator it)
    {
        if (it == end())
            return;

        removeAndInvalidateWithoutEntryConsistencyCheck(const_cast<ValueType*>(it.m_iterator.m_position));
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::removeWithoutEntryConsistencyCheck(const_iterator it)
    {
        if (it == end())
            return;

        removeAndInvalidateWithoutEntryConsistencyCheck(const_cast<ValueType*>(it.m_position));
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<ShouldValidateKey shouldValidateKey>
    inline void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::remove(const KeyType& key)
    {
        remove(find<shouldValidateKey>(key));
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline bool HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::removeIf(NOESCAPE const Invocable<bool(ValueType&)> auto& functor)
    {
        // We must use local copies in case "functor" or "deleteBucket"
        // make a function call, which prevents the compiler from keeping
        // the values in register.
        unsigned removedBucketCount = 0;
        ValueType* table = m_table;

        for (unsigned i = tableSize(); i--;) {
            ValueType& bucket = table[i];
            if (isEmptyOrDeletedBucket(bucket))
                continue;
            
            if (!functor(bucket))
                continue;
            
            deleteBucket(bucket);
            ++removedBucketCount;
        }
        if (removedBucketCount) {
            setDeletedCount(deletedCount() + removedBucketCount);
            setKeyCount(keyCount() - removedBucketCount);
        }

        if (shouldShrink())
            shrinkToBestSize();
        
        internalCheckTableConsistency();
        return removedBucketCount;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    template<size_t inlineCapacity>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::takeIf(NOESCAPE const Invocable<bool(const ValueType&)> auto& functor) -> Vector<TakeType, inlineCapacity>
    {
        // We must use local copies in case "functor" or "deleteBucket"
        // make a function call, which prevents the compiler from keeping
        // the values in register.
        unsigned removedBucketCount = 0;
        ValueType* table = m_table;
        Vector<TakeType, inlineCapacity> result;

        for (unsigned i = tableSize(); i--;) {
            ValueType& bucket = table[i];
            if (isEmptyOrDeletedBucket(bucket))
                continue;

            if (!functor(bucket))
                continue;

            result.append(ValueTraits::take(WTFMove(bucket)));
            deleteBucket(bucket);
            ++removedBucketCount;
        }
        if (removedBucketCount) {
            setDeletedCount(deletedCount() + removedBucketCount);
            setKeyCount(keyCount() - removedBucketCount);
        }

        if (shouldShrink())
            shrinkToBestSize();

        internalCheckTableConsistency();
        return result;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::allocateTable(unsigned size) -> ValueType*
    {
        static_assert(!(metadataSize % alignof(ValueType)));

        // would use a template member function with explicit specializations here, but
        // gcc doesn't appear to support that
        if constexpr (Traits::emptyValueIsZero)
            return reinterpret_cast_ptr<ValueType*>(static_cast<char*>(Malloc::zeroedMalloc(metadataSize + size * sizeof(ValueType))) + metadataSize);

        ValueType* result = reinterpret_cast_ptr<ValueType*>(static_cast<char*>(Malloc::malloc(metadataSize + size * sizeof(ValueType))) + metadataSize);
        for (unsigned i = 0; i < size; i++)
            initializeBucket(result[i]);
        return result;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::deallocateTable(ValueType* table)
    {
        unsigned size = reinterpret_cast_ptr<unsigned*>(table)[tableSizeOffset];
        for (unsigned i = 0; i < size; ++i) {
            if (!isDeletedBucket(table[i]))
                table[i].~ValueType();
        }
        Malloc::free(reinterpret_cast<char*>(table) - metadataSize);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::expand(ValueType* entry) -> ValueType*
    {
        if constexpr (KeyTraits::hasIsReleasedWeakValueFunction)
            deleteReleasedWeakBuckets();

        unsigned newSize;
        unsigned oldSize = tableSize();
        if (!oldSize)
            newSize = KeyTraits::minimumTableSize;
        else if (mustRehashInPlace())
            newSize = oldSize;
        else
            newSize = oldSize * 2;

        return rehash(newSize, entry);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    constexpr unsigned HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::computeBestTableSize(unsigned keyCount)
    {
        unsigned bestTableSize = roundUpToPowerOfTwo(keyCount);
        static constexpr double minLoadRatio = 1.0 / minLoad;

        if (HashTableSizePolicy::shouldExpand(keyCount, bestTableSize))
            bestTableSize *= 2;

        auto aboveThresholdForEagerExpansion = [](double loadFactor, unsigned keyCount, unsigned tableSize)
        {
            // Here is the rationale behind this calculation, using 3/4 load-factor.
            // With maxLoad at 3/4 and minLoad at 1/6, our average load is 11/24.
            // If we are getting half-way between 11/24 and 3/4, we double the size
            // to avoid being too close to loadMax and bring the ratio close to 11/24. This
            // give us a load in the bounds [9/24, 15/24).
            double maxLoadRatio = loadFactor;
            double averageLoadRatio = std::midpoint(minLoadRatio, maxLoadRatio);
            double halfWayBetweenAverageAndMaxLoadRatio = std::midpoint(averageLoadRatio, maxLoadRatio);
            return keyCount >= tableSize * halfWayBetweenAverageAndMaxLoadRatio;
        };

        if (bestTableSize <= maxSmallTableCapacity) {
            constexpr double smallLoadFactor = static_cast<double>(smallMaxLoadNumerator) / smallMaxLoadDenominator;
            if (aboveThresholdForEagerExpansion(smallLoadFactor, keyCount, bestTableSize))
                bestTableSize *= 2;
        } else {
            constexpr double largeLoadFactor = static_cast<double>(largeMaxLoadNumerator) / largeMaxLoadDenominator;
            if (aboveThresholdForEagerExpansion(largeLoadFactor, keyCount, bestTableSize))
                bestTableSize *= 2;
        }
        return std::max(bestTableSize, KeyTraits::minimumTableSize);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::shrinkToBestSize()
    {
        rehash(computeBestTableSize(keyCount()), nullptr);
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::deleteReleasedWeakBuckets()
    {
        unsigned tableSize = this->tableSize();
        for (unsigned i = 0; i < tableSize; ++i) {
            auto& entry = m_table[i];
            if (isReleasedWeakBucket(entry)) {
                deleteBucket(entry);
                setDeletedCount(deletedCount() + 1);
                setKeyCount(keyCount() - 1);
            }
        }
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::rehash(unsigned newTableSize, ValueType* entry) -> ValueType*
    {
        internalCheckTableConsistencyExceptSize();

        unsigned oldTableSize = tableSize();
        ValueType* oldTable = m_table;

#if DUMP_HASHTABLE_STATS
        if (oldTableSize != 0)
            ++HashTableStats::numRehashes;
#endif

#if DUMP_HASHTABLE_STATS_PER_TABLE
        if (oldTableSize != 0)
            ++m_stats->numRehashes;
#endif

        unsigned oldKeyCount = keyCount();
        m_table = allocateTable(newTableSize);
        setTableSize(newTableSize);
        setTableSizeMask(newTableSize - 1);
        setDeletedCount(0);
        setKeyCount(oldKeyCount);

        Value* newEntry = nullptr;
        for (unsigned i = 0; i != oldTableSize; ++i) {
            auto& oldEntry = oldTable[i];
            if (isDeletedBucket(oldEntry)) {
                ASSERT(std::addressof(oldEntry) != entry);
                continue;
            }

            if (isEmptyBucket(oldEntry)) {
                ASSERT(std::addressof(oldEntry) != entry);
                oldTable[i].~ValueType();
                continue;
            }

            if (isReleasedWeakBucket(oldEntry)) {
                ASSERT(std::addressof(oldEntry) != entry);
                oldEntry.~ValueType();
                setKeyCount(keyCount() - 1);
                continue;
            }

            Value* reinsertedEntry = reinsert(WTFMove(oldEntry));
            oldEntry.~ValueType();
            if (std::addressof(oldEntry) == entry) {
                ASSERT(!newEntry);
                newEntry = reinsertedEntry;
            }
        }

        if (oldTable)
            Malloc::free(reinterpret_cast<char*>(oldTable) - metadataSize);

        internalCheckTableConsistency();
        return newEntry;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::clear()
    {
        invalidateIterators(this);
        if (!m_table)
            return;

        deallocateTable(std::exchange(m_table, nullptr));
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::HashTable(const HashTable& other)
        : m_table(nullptr)
#if CHECK_HASHTABLE_ITERATORS
        , m_iterators(nullptr)
        , m_mutex(makeUnique<Lock>())
#endif
#if DUMP_HASHTABLE_STATS_PER_TABLE
        , m_stats(makeUnique<Stats>(*other.m_stats))
#endif
    {
        unsigned otherKeyCount = other.size();
        if (!otherKeyCount)
            return;

        unsigned bestTableSize = computeBestTableSize(otherKeyCount);
        m_table = allocateTable(bestTableSize);
        setTableSize(bestTableSize);
        setTableSizeMask(bestTableSize - 1);
        setKeyCount(otherKeyCount);
        setDeletedCount(0);

        for (const auto& otherValue : other)
            addUniqueForInitialization<IdentityTranslatorType>(Extractor::extract(otherValue), [&]() ALWAYS_INLINE_LAMBDA { return otherValue; });
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::swap(HashTable& other)
    {
        invalidateIterators(this);
        invalidateIterators(&other);

        std::swap(m_table, other.m_table);

#if DUMP_HASHTABLE_STATS_PER_TABLE
        m_stats.swap(other.m_stats);
#endif
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::operator=(const HashTable& other) -> HashTable&
    {
        HashTable tmp(other);
        swap(tmp);
        return *this;
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::HashTable(HashTable&& other)
#if CHECK_HASHTABLE_ITERATORS
        : m_iterators(nullptr)
        , m_mutex(makeUnique<Lock>())
#endif
    {
        invalidateIterators(&other);

        m_table = std::exchange(other.m_table, nullptr);

#if DUMP_HASHTABLE_STATS_PER_TABLE
        m_stats = WTFMove(other.m_stats);
        other.m_stats = nullptr;
#endif
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    inline auto HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::operator=(HashTable&& other) -> HashTable&
    {
        HashTable temp = WTFMove(other);
        swap(temp);
        return *this;
    }

#if ASSERT_ENABLED

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::checkTableConsistency() const
    {
        checkTableConsistencyExceptSize();
        ASSERT(!m_table || !shouldExpand());
        ASSERT(!shouldShrink());
    }

    template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
    void HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>::checkTableConsistencyExceptSize() const
    {
        if (!m_table)
            return;

        unsigned count = 0;
        unsigned deletedCount = 0;
        unsigned tableSize = this->tableSize();
        for (unsigned j = 0; j < tableSize; ++j) {
            ValueType* entry = m_table + j;
            if (isEmptyBucket(*entry))
                continue;

            if (isDeletedBucket(*entry)) {
                ++deletedCount;
                continue;
            }

            auto& key = Extractor::extract(*entry);
            const_iterator it = find<ShouldValidateKey::No>(key);
            ASSERT(entry == it.m_position);
            ++count;

            ValueCheck<Key>::checkConsistency(key);
        }

        ASSERT(count == keyCount());
        ASSERT(deletedCount == this->deletedCount());
        ASSERT(this->tableSize() >= KeyTraits::minimumTableSize);
        ASSERT(tableSizeMask());
        ASSERT(this->tableSize() == tableSizeMask() + 1);
    }

#endif // ASSERT_ENABLED

#if CHECK_HASHTABLE_ITERATORS

    template<typename HashTableType>
    void invalidateIterators(const HashTableType* table)
    {
        Locker locker { *table->m_mutex };
        typename HashTableType::const_iterator* next;
        for (auto* p = table->m_iterators; p; p = next) {
            next = p->m_next;
            p->m_table = nullptr;
            p->m_next = nullptr;
            p->m_previous = nullptr;
        }
        table->m_iterators = nullptr;
    }

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    void addIterator(const HashTableType* table, HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>* it)
    {
        it->m_table = table;
        it->m_previous = nullptr;

        // Insert iterator at head of doubly-linked list of iterators.
        if (!table) {
            it->m_next = nullptr;
        } else {
            Locker locker { *table->m_mutex };
            ASSERT(table->m_iterators != it);
            it->m_next = table->m_iterators;
            table->m_iterators = it;
            if (it->m_next) {
                ASSERT(!it->m_next->m_previous);
                it->m_next->m_previous = it;
            }
        }
    }

    template<typename HashTableType, typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits>
    void removeIterator(HashTableConstIterator<HashTableType, Key, Value, Extractor, HashFunctions, Traits, KeyTraits>* it)
    {
        // Delete iterator from doubly-linked list of iterators.
        if (!it->m_table) {
            ASSERT(!it->m_next);
            ASSERT(!it->m_previous);
        } else {
            Locker locker { *it->m_table->m_mutex };
            if (it->m_next) {
                ASSERT(it->m_next->m_previous == it);
                it->m_next->m_previous = it->m_previous;
            }
            if (it->m_previous) {
                ASSERT(it->m_table->m_iterators != it);
                ASSERT(it->m_previous->m_next == it);
                it->m_previous->m_next = it->m_next;
            } else {
                ASSERT(it->m_table->m_iterators == it);
                it->m_table->m_iterators = it->m_next;
            }
        }

        it->m_table = nullptr;
        it->m_next = nullptr;
        it->m_previous = nullptr;
    }

#endif // CHECK_HASHTABLE_ITERATORS

    struct HashTableTraits {
        template<typename Key, typename Value, typename Extractor, typename HashFunctions, typename Traits, typename KeyTraits, typename Malloc>
        using TableType = HashTable<Key, Value, Extractor, HashFunctions, Traits, KeyTraits, Malloc>;
    };

    // iterator adapters

    template<typename HashTableType, typename ValueType> struct HashTableConstIteratorAdapter {
        using iterator_category = std::forward_iterator_tag;
        using value_type = ValueType;
        using difference_type = ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        HashTableConstIteratorAdapter() = default;
        HashTableConstIteratorAdapter(const typename HashTableType::const_iterator& impl) : m_impl(impl) {}

        const ValueType* get() const { return (const ValueType*)m_impl.get(); }
        const ValueType& operator*() const { return *get(); }
        const ValueType* operator->() const { return get(); }

        HashTableConstIteratorAdapter& operator++() { ++m_impl; return *this; }
        HashTableConstIteratorAdapter& operator++(int) { auto result = *this; ++m_impl; return result; }

        typename HashTableType::const_iterator m_impl;
    };

    template<typename HashTableType, typename ValueType> struct HashTableIteratorAdapter {
        using iterator_category = std::forward_iterator_tag;
        using value_type = ValueType;
        using difference_type = ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;

        HashTableIteratorAdapter() = default;
        HashTableIteratorAdapter(const typename HashTableType::iterator& impl) : m_impl(impl) {}

        ValueType* get() const { return (ValueType*)m_impl.get(); }
        ValueType& operator*() const { return *get(); }
        ValueType* operator->() const { return get(); }

        HashTableIteratorAdapter& operator++() { ++m_impl; return *this; }
        HashTableIteratorAdapter& operator++(int) { auto result = *this; ++m_impl; return result; }

        operator HashTableConstIteratorAdapter<HashTableType, ValueType>() {
            typename HashTableType::const_iterator i = m_impl;
            return i;
        }

        typename HashTableType::iterator m_impl;
    };

    template<typename T, typename U>
    inline bool operator==(const HashTableConstIteratorAdapter<T, U>& a, const HashTableConstIteratorAdapter<T, U>& b)
    {
        return a.m_impl == b.m_impl;
    }

    template<typename T, typename U>
    inline bool operator==(const HashTableIteratorAdapter<T, U>& a, const HashTableIteratorAdapter<T, U>& b)
    {
        return a.m_impl == b.m_impl;
    }

    template<typename T, typename U>
    inline bool operator==(const HashTableConstIteratorAdapter<T, U>& a, const HashTableIteratorAdapter<T, U>& b)
    {
        return a.m_impl == b.m_impl;
    }

    template<typename T, typename U>
    inline bool operator==(const HashTableIteratorAdapter<T, U>& a, const HashTableConstIteratorAdapter<T, U>& b)
    {
        return a.m_impl == b.m_impl;
    }

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#include <wtf/HashIterators.h>
