#pragma once

#include <Common/HashTable/Hash.h>
#include <Common/HashTable/HashTable.h>
#include <Common/HashTable/HashTableAllocator.h>

#include <Columns/ColumnString.h>
#include <ext/range.h>

namespace DB
{

namespace
{
    template <typename ColumnType, bool with_saved_hash>
    struct ReverseIndexHashTableState;

    template <typename ColumnType>
    struct ReverseIndexHashTableState<ColumnType, /* with_saved_hash = */ false>
    {
        ColumnType * index_column;
    };

    template <typename ColumnType>
    struct ReverseIndexHashTableState<ColumnType, /* with_saved_hash = */ true>
    {
        ColumnType * index_column;
        typename ColumnVector<UInt64>::Container * saved_hash_column;
    };


    template <typename Hash>
    struct ReverseIndexHash : public Hash
    {
        template <typename T>
        size_t operator()(T key)
        {
            throw Exception("operator()(key) is not implemented for ReverseIndexHash.", ErrorCodes::LOGICAL_ERROR);
        }

        template <typename State, typename T>
        size_t operator()(const State & state, T key)
        {
            return Hash::operator()(state.index_column->getElement(key));
        }
    };

    using ReverseIndexStringHash = ReverseIndexHash<StringRefHash>;

    template <typename IndexType>
    using ReverseIndexNumberHash = ReverseIndexHash<DefaultHash<IndexType>>;


    template <typename IndexType, typename Hash, typename HashTable, typename ColumnType, bool string_hash>
    struct  ReverseIndexHashTableCell
        : public HashTableCell<IndexType, Hash, ReverseIndexHashTableState<ColumnType, string_hash>>
    {
        using Base = HashTableCell<IndexType, Hash, ReverseIndexHashTableState<ColumnType, string_hash>>;
        using State = typename Base::State;
        using Base::key;

        static constexpr bool need_zero_value_storage = false;

        /// Special case when we want to compare with something not in index_column.
        /// When we compare something inside column default keyEquals checks only that row numbers are equal.
        /// ObjectToCompare is StringRef for strings and IndexType for numbers.
        template <typename ObjectToCompare>
        bool keyEquals(const ObjectToCompare & object, size_t hash_, const State & state) const
        {
            if constexpr (string_hash)
                return hash_ == (*state.saved_hash_column)[key] && object == state.index_column->getDataAt(key);
            else
                return object == state.index_column->getElement(key);
        }

        size_t getHash(const Hash & hash) const
        {
            /// Hack. HashTable is Hash itself.
            const auto & state = static_cast<const State &>(static_cast<const HashTable &>(hash));
            if (string_hash)
                return (*state.saved_hash_column)[key];
            else
                return hash(state, key);
        }
    };


    template <typename IndexType, typename ColumnType>
    class ReverseIndexStringHashTable : public HashTable<
            IndexType,
            ReverseIndexHashTableCell<
                    IndexType,
                    ReverseIndexStringHash,
                    ReverseIndexStringHashTable<IndexType, ColumnType>,
                    ColumnType,
                    true>,
            ReverseIndexStringHash,
            HashTableGrower<>,
            HashTableAllocator> {};

    template <typename IndexType, typename ColumnType>
    class ReverseIndexNumberHashTable : public HashTable<
            IndexType,
            ReverseIndexHashTableCell<
                    IndexType,
                    ReverseIndexNumberHash<typename ColumnType::value_type>,
                    ReverseIndexNumberHashTable<IndexType, ColumnType>,
                    ColumnType,
                    false>,
            ReverseIndexNumberHash<typename ColumnType::value_type>,
            HashTableGrower<>,
            HashTableAllocator> {};


    template <typename IndexType, typename ColumnType, bool is_numeric_column>
    struct SelectReverseIndexHashTable;

    template <typename IndexType, typename ColumnType>
    struct SelectReverseIndexHashTable<IndexType, ColumnType, true>
    {
        using Type = ReverseIndexNumberHashTable<IndexType, ColumnType>;
    };

    template <typename IndexType, typename ColumnType>
    struct SelectReverseIndexHashTable<IndexType, ColumnType, false>
    {
        using Type = ReverseIndexStringHashTable<IndexType, ColumnType>;
    };


    template <typename T>
    constexpr bool isNumericColumn(const T *) { return false; }

    template <typename T>
    constexpr bool isNumericColumn(const ColumnVector<T> *) { return true; }

    static_assert(isNumericColumn(static_cast<ColumnVector<UInt8> *>(nullptr)));
    static_assert(!isNumericColumn(static_cast<ColumnString *>(nullptr)));


    template <typename IndexType, typename ColumnType>
    using ReverseIndexHashTable = typename SelectReverseIndexHashTable<IndexType, ColumnType,
            isNumericColumn(static_cast<ColumnType *>(nullptr))>::Type;
}


template <typename IndexType, typename ColumnType>
class ReverseIndex
{
public:
    explicit ReverseIndex(size_t num_prefix_rows_to_skip) : num_prefix_rows_to_skip(num_prefix_rows_to_skip) {}

    void setColumn(ColumnType * column_) { column = column_; }

    static constexpr bool is_numeric_column = isNumericColumn(static_cast<ColumnType *>(nullptr));
    static constexpr bool use_saved_hash = is_numeric_column;

    UInt64 insert(UInt64 from_position);
    UInt64 insertFromLastRow();
    UInt64 getInsertionPoint(const StringRef & data);

    ColumnType * getColumn() const { return column; }
    size_t size() const;

    size_t allocatedBytes() const { return index ? index->getBufferSizeInBytes() : 0; }

private:
    ColumnType * column = nullptr;
    size_t num_prefix_rows_to_skip;

    using IndexMapType = ReverseIndexHashTable<IndexType, ColumnType>;

    /// Lazy initialized.
    std::unique_ptr<IndexMapType> index;
    ColumnUInt64::MutablePtr saved_hash;

    void buildIndex();
};


template <typename IndexType, typename ColumnType>
size_t ReverseIndex<IndexType, ColumnType>::size() const
{
    if (!column)
        throw Exception("ReverseIndex has not size because index column wasn't set.", ErrorCodes::LOGICAL_ERROR);

    return column->size();
}

template <typename IndexType, typename ColumnType>
void ReverseIndex<IndexType, ColumnType>::buildIndex()
{
    if (index)
        return;

    if (!column)
        throw Exception("ReverseIndex can't build index because index column wasn't set.", ErrorCodes::LOGICAL_ERROR);

    auto size = column->size();
    index = std::make_unique<IndexMapType>(size);

    if constexpr (use_saved_hash)
        saved_hash = ColumnUInt64::create(size);

    auto & state = static_cast<typename IndexMapType::cell_type::State &>(*index);
    state.index_column = column;
    if constexpr (use_saved_hash)
        state.saved_hash_column = saved_hash.get();

    using IteratorType = typename IndexMapType::iterator;
    IteratorType iterator;
    bool inserted;

    for (auto row : ext::range(num_prefix_rows_to_skip, size))
    {
        if constexpr (use_saved_hash)
        {
            auto hash = StringRefHash()(column->getDataAt(row));
            index->emplace(row, iterator, inserted, hash);
        }
        else
            index->emplace(row, iterator, inserted);

        if (!inserted)
            throw Exception("Duplicating keys found in ReverseIndex.", ErrorCodes::LOGICAL_ERROR);
    }
}

template <typename IndexType, typename ColumnType>
UInt64 ReverseIndex<IndexType, ColumnType>::insert(UInt64 from_position)
{
    if (!index)
        buildIndex();

    using IteratorType = typename IndexMapType::iterator;
    IteratorType iterator;
    bool inserted;

    if constexpr (use_saved_hash)
    {
        auto hash = StringRefHash()(column->getDataAt(from_position));
        index->emplace(from_position, iterator, inserted, hash);
    }
    else
        index->emplace(from_position, iterator, inserted);

    return *iterator;
}

template <typename IndexType, typename ColumnType>
UInt64 ReverseIndex<IndexType, ColumnType>::insertFromLastRow()
{
    if (!column)
        throw Exception("ReverseIndex can't insert row from column because index column wasn't set.",
                        ErrorCodes::LOGICAL_ERROR);

    UInt64 num_rows = size();

    if (num_rows == 0)
        throw Exception("ReverseIndex can't insert row from column because it is empty.", ErrorCodes::LOGICAL_ERROR);

    UInt64 position = num_rows - 1;
    UInt64 inserted_pos = insert(position);
    if (position != inserted_pos)
        throw Exception("Can't insert into reverse index from last row (" + toString(position)
                        + ")because the same row is in position " + toString(inserted_pos), ErrorCodes::LOGICAL_ERROR);

    return inserted_pos;
}

template <typename IndexType, typename ColumnType>
UInt64 ReverseIndex<IndexType, ColumnType>::getInsertionPoint(const StringRef & data)
{
    if (!index)
        buildIndex();

    UInt64 hash;
    if constexpr (is_numeric_column)
    {
        using ValueType = typename ColumnType::value_type;
        hash = DefaultHash<ValueType>()(*static_cast<ValueType *>(data.data));
    }
    else
        hash = StringRefHash()(data);

    auto iterator = index->find(data, hash);
    return iterator == index->end() ? size() : *iterator;
}

}
