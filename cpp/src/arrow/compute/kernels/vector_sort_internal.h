// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

#include "arrow/array.h"
#include "arrow/compute/api_vector.h"
#include "arrow/compute/kernels/chunked_internal.h"
#include "arrow/table.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"

namespace arrow::compute::internal {

// Visit all physical types for which sorting is implemented.
#define VISIT_SORTABLE_PHYSICAL_TYPES(VISIT) \
  VISIT(BooleanType)                         \
  VISIT(Int8Type)                            \
  VISIT(Int16Type)                           \
  VISIT(Int32Type)                           \
  VISIT(Int64Type)                           \
  VISIT(UInt8Type)                           \
  VISIT(UInt16Type)                          \
  VISIT(UInt32Type)                          \
  VISIT(UInt64Type)                          \
  VISIT(FloatType)                           \
  VISIT(DoubleType)                          \
  VISIT(BinaryType)                          \
  VISIT(LargeBinaryType)                     \
  VISIT(FixedSizeBinaryType)                 \
  VISIT(Decimal128Type)                      \
  VISIT(Decimal256Type)

// NOTE: std::partition is usually faster than std::stable_partition.

struct NonStablePartitioner {
  template <typename Predicate, typename IndexType>
  IndexType* operator()(IndexType* indices_begin, IndexType* indices_end,
                        Predicate&& pred) {
    return std::partition(indices_begin, indices_end, std::forward<Predicate>(pred));
  }
};

struct StablePartitioner {
  template <typename Predicate, typename IndexType>
  IndexType* operator()(IndexType* indices_begin, IndexType* indices_end,
                        Predicate&& pred) {
    return std::stable_partition(indices_begin, indices_end,
                                 std::forward<Predicate>(pred));
  }
};

template <typename TypeClass>
constexpr bool has_null_like_values() {
  return is_physical_floating(TypeClass::type_id);
}

// Compare two values, taking NaNs into account

template <typename Type, typename Value>
int CompareTypeValues(Value&& left, Value&& right, SortOrder order,
                      NullPlacement null_placement) {
  if constexpr (has_null_like_values<Type>()) {
    const bool is_nan_left = std::isnan(left);
    const bool is_nan_right = std::isnan(right);
    if (is_nan_left && is_nan_right) {
      return 0;
    } else if (is_nan_left) {
      return null_placement == NullPlacement::AtStart ? -1 : 1;
    } else if (is_nan_right) {
      return null_placement == NullPlacement::AtStart ? 1 : -1;
    }
  }
  int compared;
  if (left == right) {
    compared = 0;
  } else if (left > right) {
    compared = 1;
  } else {
    compared = -1;
  }
  if (order == SortOrder::Descending) {
    compared = -compared;
  }
  return compared;
}

template <typename IndexType>
struct GenericNullPartitionResult {
  IndexType* non_nulls_begin;
  IndexType* non_nulls_end;
  IndexType* nulls_begin;
  IndexType* nulls_end;

  IndexType* overall_begin() const { return std::min(nulls_begin, non_nulls_begin); }

  IndexType* overall_end() const { return std::max(nulls_end, non_nulls_end); }

  int64_t non_null_count() const { return non_nulls_end - non_nulls_begin; }

  int64_t null_count() const { return nulls_end - nulls_begin; }

  static GenericNullPartitionResult NoNulls(IndexType* indices_begin,
                                            IndexType* indices_end,
                                            NullPlacement null_placement) {
    if (null_placement == NullPlacement::AtStart) {
      return {indices_begin, indices_end, indices_begin, indices_begin};
    } else {
      return {indices_begin, indices_end, indices_end, indices_end};
    }
  }

  static GenericNullPartitionResult NullsOnly(IndexType* indices_begin,
                                              IndexType* indices_end,
                                              NullPlacement null_placement) {
    if (null_placement == NullPlacement::AtStart) {
      return {indices_end, indices_end, indices_begin, indices_end};
    } else {
      return {indices_begin, indices_begin, indices_begin, indices_end};
    }
  }

  static GenericNullPartitionResult NullsAtEnd(IndexType* indices_begin,
                                               IndexType* indices_end,
                                               IndexType* midpoint) {
    ARROW_DCHECK_GE(midpoint, indices_begin);
    ARROW_DCHECK_LE(midpoint, indices_end);
    return {indices_begin, midpoint, midpoint, indices_end};
  }

  static GenericNullPartitionResult NullsAtStart(IndexType* indices_begin,
                                                 IndexType* indices_end,
                                                 IndexType* midpoint) {
    ARROW_DCHECK_GE(midpoint, indices_begin);
    ARROW_DCHECK_LE(midpoint, indices_end);
    return {midpoint, indices_end, indices_begin, midpoint};
  }

  template <typename TargetIndexType>
  GenericNullPartitionResult<TargetIndexType> TranslateTo(
      IndexType* indices_begin, TargetIndexType* target_indices_begin) const {
    return {
        (non_nulls_begin - indices_begin) + target_indices_begin,
        (non_nulls_end - indices_begin) + target_indices_begin,
        (nulls_begin - indices_begin) + target_indices_begin,
        (nulls_end - indices_begin) + target_indices_begin,
    };
  }
};

using NullPartitionResult = GenericNullPartitionResult<uint64_t>;
using ChunkedNullPartitionResult = GenericNullPartitionResult<CompressedChunkLocation>;

// Move nulls (not null-like values) to end of array.
//
// `offset` is used when this is called on a chunk of a chunked array
template <typename Partitioner>
NullPartitionResult PartitionNullsOnly(uint64_t* indices_begin, uint64_t* indices_end,
                                       const Array& values, int64_t offset,
                                       NullPlacement null_placement) {
  if (values.null_count() == 0) {
    return NullPartitionResult::NoNulls(indices_begin, indices_end, null_placement);
  }
  Partitioner partitioner;
  if (null_placement == NullPlacement::AtStart) {
    auto nulls_end = partitioner(
        indices_begin, indices_end,
        [&values, &offset](uint64_t ind) { return values.IsNull(ind - offset); });
    return NullPartitionResult::NullsAtStart(indices_begin, indices_end, nulls_end);
  } else {
    auto nulls_begin = partitioner(
        indices_begin, indices_end,
        [&values, &offset](uint64_t ind) { return !values.IsNull(ind - offset); });
    return NullPartitionResult::NullsAtEnd(indices_begin, indices_end, nulls_begin);
  }
}

// Move non-null null-like values to end of array.
//
// `offset` is used when this is called on a chunk of a chunked array
template <typename ArrayType, typename Partitioner>
NullPartitionResult PartitionNullLikes(uint64_t* indices_begin, uint64_t* indices_end,
                                       const ArrayType& values, int64_t offset,
                                       NullPlacement null_placement) {
  if constexpr (has_null_like_values<typename ArrayType::TypeClass>()) {
    Partitioner partitioner;
    if (null_placement == NullPlacement::AtStart) {
      auto null_likes_end =
          partitioner(indices_begin, indices_end, [&values, &offset](uint64_t ind) {
            return std::isnan(values.GetView(ind - offset));
          });
      return NullPartitionResult::NullsAtStart(indices_begin, indices_end,
                                               null_likes_end);
    } else {
      auto null_likes_begin =
          partitioner(indices_begin, indices_end, [&values, &offset](uint64_t ind) {
            return !std::isnan(values.GetView(ind - offset));
          });
      return NullPartitionResult::NullsAtEnd(indices_begin, indices_end,
                                             null_likes_begin);
    }
  } else {
    return NullPartitionResult::NoNulls(indices_begin, indices_end, null_placement);
  }
}

// Move nulls to end of array.
//
// `offset` is used when this is called on a chunk of a chunked array
template <typename ArrayType, typename Partitioner>
NullPartitionResult PartitionNulls(uint64_t* indices_begin, uint64_t* indices_end,
                                   const ArrayType& values, int64_t offset,
                                   NullPlacement null_placement) {
  // Partition nulls at start (resp. end), and null-like values just before (resp. after)
  NullPartitionResult p = PartitionNullsOnly<Partitioner>(indices_begin, indices_end,
                                                          values, offset, null_placement);
  NullPartitionResult q = PartitionNullLikes<ArrayType, Partitioner>(
      p.non_nulls_begin, p.non_nulls_end, values, offset, null_placement);
  return NullPartitionResult{q.non_nulls_begin, q.non_nulls_end,
                             std::min(q.nulls_begin, p.nulls_begin),
                             std::max(q.nulls_end, p.nulls_end)};
}

//
// Null partitioning on chunked arrays, in two flavors:
// 1) with uint64_t indices and ChunkedArrayResolver
// 2) with CompressedChunkLocation and span of chunks
//

template <typename Partitioner>
NullPartitionResult PartitionNullsOnly(uint64_t* indices_begin, uint64_t* indices_end,
                                       const ChunkedArrayResolver& resolver,
                                       int64_t null_count, NullPlacement null_placement) {
  if (null_count == 0) {
    return NullPartitionResult::NoNulls(indices_begin, indices_end, null_placement);
  }
  Partitioner partitioner;
  if (null_placement == NullPlacement::AtStart) {
    auto nulls_end = partitioner(indices_begin, indices_end, [&](uint64_t ind) {
      const auto chunk = resolver.Resolve(ind);
      return chunk.IsNull();
    });
    return NullPartitionResult::NullsAtStart(indices_begin, indices_end, nulls_end);
  } else {
    auto nulls_begin = partitioner(indices_begin, indices_end, [&](uint64_t ind) {
      const auto chunk = resolver.Resolve(ind);
      return !chunk.IsNull();
    });
    return NullPartitionResult::NullsAtEnd(indices_begin, indices_end, nulls_begin);
  }
}

template <typename Partitioner>
ChunkedNullPartitionResult PartitionNullsOnly(CompressedChunkLocation* locations_begin,
                                              CompressedChunkLocation* locations_end,
                                              util::span<const Array* const> chunks,
                                              int64_t null_count,
                                              NullPlacement null_placement) {
  if (null_count == 0) {
    return ChunkedNullPartitionResult::NoNulls(locations_begin, locations_end,
                                               null_placement);
  }
  Partitioner partitioner;
  if (null_placement == NullPlacement::AtStart) {
    auto nulls_end =
        partitioner(locations_begin, locations_end, [&](CompressedChunkLocation loc) {
          return chunks[loc.chunk_index()]->IsNull(
              static_cast<int64_t>(loc.index_in_chunk()));
        });
    return ChunkedNullPartitionResult::NullsAtStart(locations_begin, locations_end,
                                                    nulls_end);
  } else {
    auto nulls_begin =
        partitioner(locations_begin, locations_end, [&](CompressedChunkLocation loc) {
          return !chunks[loc.chunk_index()]->IsNull(
              static_cast<int64_t>(loc.index_in_chunk()));
        });
    return ChunkedNullPartitionResult::NullsAtEnd(locations_begin, locations_end,
                                                  nulls_begin);
  }
}

template <typename ArrayType, typename Partitioner,
          typename TypeClass = typename ArrayType::TypeClass>
NullPartitionResult PartitionNullLikes(uint64_t* indices_begin, uint64_t* indices_end,
                                       const ChunkedArrayResolver& resolver,
                                       NullPlacement null_placement) {
  if constexpr (has_null_like_values<typename ArrayType::TypeClass>()) {
    Partitioner partitioner;
    if (null_placement == NullPlacement::AtStart) {
      auto null_likes_end = partitioner(indices_begin, indices_end, [&](uint64_t ind) {
        const auto chunk = resolver.Resolve(ind);
        return std::isnan(chunk.Value<TypeClass>());
      });
      return NullPartitionResult::NullsAtStart(indices_begin, indices_end,
                                               null_likes_end);
    } else {
      auto null_likes_begin = partitioner(indices_begin, indices_end, [&](uint64_t ind) {
        const auto chunk = resolver.Resolve(ind);
        return !std::isnan(chunk.Value<TypeClass>());
      });
      return NullPartitionResult::NullsAtEnd(indices_begin, indices_end,
                                             null_likes_begin);
    }
  } else {
    return NullPartitionResult::NoNulls(indices_begin, indices_end, null_placement);
  }
}

template <typename ArrayType, typename Partitioner>
NullPartitionResult PartitionNulls(uint64_t* indices_begin, uint64_t* indices_end,
                                   const ChunkedArrayResolver& resolver,
                                   int64_t null_count, NullPlacement null_placement) {
  // Partition nulls at start (resp. end), and null-like values just before (resp. after)
  NullPartitionResult p = PartitionNullsOnly<Partitioner>(
      indices_begin, indices_end, resolver, null_count, null_placement);
  NullPartitionResult q = PartitionNullLikes<ArrayType, Partitioner>(
      p.non_nulls_begin, p.non_nulls_end, resolver, null_placement);
  return NullPartitionResult{q.non_nulls_begin, q.non_nulls_end,
                             std::min(q.nulls_begin, p.nulls_begin),
                             std::max(q.nulls_end, p.nulls_end)};
}

template <typename IndexType, typename NullPartitionResultType>
struct GenericMergeImpl {
  using MergeNullsFunc = std::function<void(IndexType* nulls_begin,
                                            IndexType* nulls_middle, IndexType* nulls_end,
                                            IndexType* temp_indices, int64_t null_count)>;

  using MergeNonNullsFunc =
      std::function<void(IndexType* range_begin, IndexType* range_middle,
                         IndexType* range_end, IndexType* temp_indices)>;

  GenericMergeImpl(NullPlacement null_placement, MergeNullsFunc&& merge_nulls,
                   MergeNonNullsFunc&& merge_non_nulls)
      : null_placement_(null_placement),
        merge_nulls_(std::move(merge_nulls)),
        merge_non_nulls_(std::move(merge_non_nulls)) {}

  Status Init(ExecContext* ctx, int64_t temp_indices_length) {
    ARROW_ASSIGN_OR_RAISE(
        temp_buffer_,
        AllocateBuffer(sizeof(IndexType) * temp_indices_length, ctx->memory_pool()));
    temp_indices_ = reinterpret_cast<IndexType*>(temp_buffer_->mutable_data());
    return Status::OK();
  }

  NullPartitionResultType Merge(const NullPartitionResultType& left,
                                const NullPartitionResultType& right,
                                int64_t null_count) const {
    if (null_placement_ == NullPlacement::AtStart) {
      return MergeNullsAtStart(left, right, null_count);
    } else {
      return MergeNullsAtEnd(left, right, null_count);
    }
  }

  NullPartitionResultType MergeNullsAtStart(const NullPartitionResultType& left,
                                            const NullPartitionResultType& right,
                                            int64_t null_count) const {
    // Input layout:
    // [left nulls .... left non-nulls .... right nulls .... right non-nulls]
    ARROW_DCHECK_EQ(left.nulls_end, left.non_nulls_begin);
    ARROW_DCHECK_EQ(left.non_nulls_end, right.nulls_begin);
    ARROW_DCHECK_EQ(right.nulls_end, right.non_nulls_begin);

    // Mutate the input, stably, to obtain the following layout:
    // [left nulls .... right nulls .... left non-nulls .... right non-nulls]
    std::rotate(left.non_nulls_begin, right.nulls_begin, right.nulls_end);

    const auto p = NullPartitionResultType::NullsAtStart(
        left.nulls_begin, right.non_nulls_end,
        left.nulls_begin + left.null_count() + right.null_count());

    // If the type has null-like values (such as NaN), ensure those plus regular
    // nulls are partitioned in the right order.  Note this assumes that all
    // null-like values (e.g. NaN) are ordered equally.
    if (p.null_count()) {
      merge_nulls_(p.nulls_begin, p.nulls_begin + left.null_count(), p.nulls_end,
                   temp_indices_, null_count);
    }

    // Merge the non-null values into temp area
    ARROW_DCHECK_EQ(right.non_nulls_begin - p.non_nulls_begin, left.non_null_count());
    ARROW_DCHECK_EQ(p.non_nulls_end - right.non_nulls_begin, right.non_null_count());
    if (p.non_null_count()) {
      merge_non_nulls_(p.non_nulls_begin, right.non_nulls_begin, p.non_nulls_end,
                       temp_indices_);
    }
    return p;
  }

  NullPartitionResultType MergeNullsAtEnd(const NullPartitionResultType& left,
                                          const NullPartitionResultType& right,
                                          int64_t null_count) const {
    // Input layout:
    // [left non-nulls .... left nulls .... right non-nulls .... right nulls]
    ARROW_DCHECK_EQ(left.non_nulls_end, left.nulls_begin);
    ARROW_DCHECK_EQ(left.nulls_end, right.non_nulls_begin);
    ARROW_DCHECK_EQ(right.non_nulls_end, right.nulls_begin);

    // Mutate the input, stably, to obtain the following layout:
    // [left non-nulls .... right non-nulls .... left nulls .... right nulls]
    std::rotate(left.nulls_begin, right.non_nulls_begin, right.non_nulls_end);

    const auto p = NullPartitionResultType::NullsAtEnd(
        left.non_nulls_begin, right.nulls_end,
        left.non_nulls_begin + left.non_null_count() + right.non_null_count());

    // If the type has null-like values (such as NaN), ensure those plus regular
    // nulls are partitioned in the right order.  Note this assumes that all
    // null-like values (e.g. NaN) are ordered equally.
    if (p.null_count()) {
      merge_nulls_(p.nulls_begin, p.nulls_begin + left.null_count(), p.nulls_end,
                   temp_indices_, null_count);
    }

    // Merge the non-null values into temp area
    ARROW_DCHECK_EQ(left.non_nulls_end - p.non_nulls_begin, left.non_null_count());
    ARROW_DCHECK_EQ(p.non_nulls_end - left.non_nulls_end, right.non_null_count());
    if (p.non_null_count()) {
      merge_non_nulls_(p.non_nulls_begin, left.non_nulls_end, p.non_nulls_end,
                       temp_indices_);
    }
    return p;
  }

 private:
  NullPlacement null_placement_;
  MergeNullsFunc merge_nulls_;
  MergeNonNullsFunc merge_non_nulls_;
  std::unique_ptr<Buffer> temp_buffer_;
  IndexType* temp_indices_ = nullptr;
};

using MergeImpl = GenericMergeImpl<uint64_t, NullPartitionResult>;
using ChunkedMergeImpl =
    GenericMergeImpl<CompressedChunkLocation, ChunkedNullPartitionResult>;

// TODO make this usable if indices are non trivial on input
// (see ConcreteRecordBatchColumnSorter)
// `offset` is used when this is called on a chunk of a chunked array
using ArraySortFunc = std::function<Result<NullPartitionResult>(
    uint64_t* indices_begin, uint64_t* indices_end, const Array& values, int64_t offset,
    const ArraySortOptions& options, ExecContext* ctx)>;

Result<ArraySortFunc> GetArraySorter(const DataType& type);

Result<NullPartitionResult> SortChunkedArray(ExecContext* ctx, uint64_t* indices_begin,
                                             uint64_t* indices_end,
                                             const ChunkedArray& chunked_array,
                                             SortOrder sort_order,
                                             NullPlacement null_placement);

Result<NullPartitionResult> SortChunkedArray(
    ExecContext* ctx, uint64_t* indices_begin, uint64_t* indices_end,
    const std::shared_ptr<DataType>& physical_type, const ArrayVector& physical_chunks,
    SortOrder sort_order, NullPlacement null_placement);

Result<NullPartitionResult> SortStructArray(ExecContext* ctx, uint64_t* indices_begin,
                                            uint64_t* indices_end,
                                            const StructArray& array,
                                            SortOrder sort_order,
                                            NullPlacement null_placement);

// ----------------------------------------------------------------------
// Helpers for Sort/SelectK/Rank implementations

struct SortField {
  SortField() = default;
  SortField(FieldPath path, SortOrder order, const DataType* type)
      : path(std::move(path)), order(order), type(type) {}
  SortField(int index, SortOrder order, const DataType* type)
      : SortField(FieldPath({index}), order, type) {}

  bool is_nested() const { return path.indices().size() > 1; }

  FieldPath path;
  SortOrder order;
  const DataType* type;
};

inline Status CheckNonNested(const FieldRef& ref) {
  if (ref.IsNested()) {
    return Status::KeyError("Nested keys not supported for SortKeys");
  }
  return Status::OK();
}

template <typename T>
Result<T> PrependInvalidColumn(Result<T> res) {
  if (res.ok()) return res;
  return res.status().WithMessage("Invalid sort key column: ", res.status().message());
}

// Return the field indices of the sort keys, deduplicating them along the way
Result<std::vector<SortField>> FindSortKeys(const Schema& schema,
                                            const std::vector<SortKey>& sort_keys);

template <typename ResolvedSortKey, typename ResolvedSortKeyFactory>
Result<std::vector<ResolvedSortKey>> ResolveSortKeys(
    const Schema& schema, const std::vector<SortKey>& sort_keys,
    ResolvedSortKeyFactory&& factory) {
  ARROW_ASSIGN_OR_RAISE(const auto fields, FindSortKeys(schema, sort_keys));
  std::vector<ResolvedSortKey> resolved;
  resolved.reserve(fields.size());
  for (const auto& f : fields) {
    ARROW_ASSIGN_OR_RAISE(auto resolved_key, factory(f));
    resolved.push_back(std::move(resolved_key));
  }
  return resolved;
}

template <typename ResolvedSortKey, typename TableOrBatch>
Result<std::vector<ResolvedSortKey>> ResolveSortKeys(
    const TableOrBatch& table_or_batch, const std::vector<SortKey>& sort_keys) {
  return ResolveSortKeys<ResolvedSortKey>(
      *table_or_batch.schema(), sort_keys,
      [&](const SortField& f) -> Result<ResolvedSortKey> {
        if (f.is_nested()) {
          // TODO: Some room for improvement here, as we potentially duplicate some of the
          // null-flattening work for nested sort keys. For instance, given two keys with
          // paths [0,0,0,0] and [0,0,0,1], we shouldn't need to flatten the first three
          // components more than once.
          ARROW_ASSIGN_OR_RAISE(auto child, f.path.GetFlattened(table_or_batch));
          return ResolvedSortKey{std::move(child), f.order};
        }
        return ResolvedSortKey{table_or_batch.column(f.path[0]), f.order};
      });
}

// // Returns an error status if no column matching `ref` is found, or if the FieldRef is
// // a nested reference.
inline Result<std::shared_ptr<ChunkedArray>> GetColumn(const Table& table,
                                                       const FieldRef& ref) {
  RETURN_NOT_OK(CheckNonNested(ref));
  ARROW_ASSIGN_OR_RAISE(auto path, ref.FindOne(*table.schema()));
  return table.column(path[0]);
}

inline Result<std::shared_ptr<Array>> GetColumn(const RecordBatch& batch,
                                                const FieldRef& ref) {
  RETURN_NOT_OK(CheckNonNested(ref));
  return ref.GetOne(batch);
}

// We could try to reproduce the concrete Array classes' facilities
// (such as cached raw values pointer) in a separate hierarchy of
// physical accessors, but doing so ends up too cumbersome.
// Instead, we simply create the desired concrete Array objects.
inline std::shared_ptr<Array> GetPhysicalArray(
    const Array& array, const std::shared_ptr<DataType>& physical_type) {
  auto new_data = array.data()->Copy();
  new_data->type = physical_type;
  return MakeArray(std::move(new_data));
}

inline ArrayVector GetPhysicalChunks(const ArrayVector& chunks,
                                     const std::shared_ptr<DataType>& physical_type) {
  ArrayVector physical(chunks.size());
  std::transform(chunks.begin(), chunks.end(), physical.begin(),
                 [&](const std::shared_ptr<Array>& array) {
                   return GetPhysicalArray(*array, physical_type);
                 });
  return physical;
}

inline ArrayVector GetPhysicalChunks(const ChunkedArray& chunked_array,
                                     const std::shared_ptr<DataType>& physical_type) {
  return GetPhysicalChunks(chunked_array.chunks(), physical_type);
}

// Compare two records in a single column (either from a batch or table)
template <typename ResolvedSortKey>
struct ColumnComparator {
  using Location = typename ResolvedSortKey::LocationType;

  ColumnComparator(const ResolvedSortKey& sort_key, NullPlacement null_placement)
      : sort_key_(sort_key), null_placement_(null_placement) {}

  virtual ~ColumnComparator() = default;

  virtual int Compare(const Location& left, const Location& right) const = 0;

  ResolvedSortKey sort_key_;
  NullPlacement null_placement_;
};

template <typename ResolvedSortKey, typename Type>
struct ConcreteColumnComparator : public ColumnComparator<ResolvedSortKey> {
  using Location = typename ResolvedSortKey::LocationType;

  using ColumnComparator<ResolvedSortKey>::ColumnComparator;

  int Compare(const Location& left, const Location& right) const override {
    const auto& sort_key = this->sort_key_;

    const auto chunk_left = sort_key.GetChunk(left);
    const auto chunk_right = sort_key.GetChunk(right);
    if (sort_key.null_count > 0) {
      const bool is_null_left = chunk_left.IsNull();
      const bool is_null_right = chunk_right.IsNull();
      if (is_null_left && is_null_right) {
        return 0;
      } else if (is_null_left) {
        return this->null_placement_ == NullPlacement::AtStart ? -1 : 1;
      } else if (is_null_right) {
        return this->null_placement_ == NullPlacement::AtStart ? 1 : -1;
      }
    }
    return CompareTypeValues<Type>(chunk_left.template Value<Type>(),
                                   chunk_right.template Value<Type>(), sort_key.order,
                                   this->null_placement_);
  }
};

template <typename ResolvedSortKey>
struct ConcreteColumnComparator<ResolvedSortKey, NullType>
    : public ColumnComparator<ResolvedSortKey> {
  using Location = typename ResolvedSortKey::LocationType;

  using ColumnComparator<ResolvedSortKey>::ColumnComparator;

  int Compare(const Location& left, const Location& right) const override { return 0; }
};

// Compare two records in the same RecordBatch or Table
// (indexing is handled through ResolvedSortKey)
template <typename ResolvedSortKey>
class MultipleKeyComparator {
 public:
  using Location = typename ResolvedSortKey::LocationType;

  MultipleKeyComparator(const std::vector<ResolvedSortKey>& sort_keys,
                        NullPlacement null_placement)
      : sort_keys_(sort_keys), null_placement_(null_placement) {
    status_ &= MakeComparators();
  }

  Status status() const { return status_; }

  // Returns true if the left-th value should be ordered before the
  // right-th value, false otherwise. The start_sort_key_index-th
  // sort key and subsequent sort keys are used for comparison.
  bool Compare(const Location& left, const Location& right, size_t start_sort_key_index) {
    return CompareInternal(left, right, start_sort_key_index) < 0;
  }

  bool Equals(const Location& left, const Location& right, size_t start_sort_key_index) {
    return CompareInternal(left, right, start_sort_key_index) == 0;
  }

 private:
  struct ColumnComparatorFactory {
#define VISIT(TYPE) \
  Status Visit(const TYPE& type) { return VisitGeneric(type); }

    VISIT_SORTABLE_PHYSICAL_TYPES(VISIT)
    VISIT(NullType)

#undef VISIT

    Status Visit(const DataType& type) {
      return Status::TypeError("Unsupported type for batch or table sorting: ",
                               type.ToString());
    }

    template <typename Type>
    Status VisitGeneric(const Type& type) {
      res.reset(
          new ConcreteColumnComparator<ResolvedSortKey, Type>{sort_key, null_placement});
      return Status::OK();
    }

    const ResolvedSortKey& sort_key;
    NullPlacement null_placement;
    std::unique_ptr<ColumnComparator<ResolvedSortKey>> res;
  };

  Status MakeComparators() {
    column_comparators_.reserve(sort_keys_.size());

    for (const auto& sort_key : sort_keys_) {
      ColumnComparatorFactory factory{sort_key, null_placement_, nullptr};
      RETURN_NOT_OK(VisitTypeInline(*sort_key.type, &factory));
      column_comparators_.push_back(std::move(factory.res));
    }
    return Status::OK();
  }

  // Compare two records in the same table and return -1, 0 or 1.
  //
  // -1: The left is less than the right.
  // 0: The left equals to the right.
  // 1: The left is greater than the right.
  //
  // This supports null and NaN. Null is processed in this and NaN
  // is processed in CompareTypeValue().
  int CompareInternal(const Location& left, const Location& right,
                      size_t start_sort_key_index) {
    const auto num_sort_keys = sort_keys_.size();
    for (size_t i = start_sort_key_index; i < num_sort_keys; ++i) {
      const int r = column_comparators_[i]->Compare(left, right);
      if (r != 0) {
        return r;
      }
    }
    return 0;
  }

  const std::vector<ResolvedSortKey>& sort_keys_;
  const NullPlacement null_placement_;
  std::vector<std::unique_ptr<ColumnComparator<ResolvedSortKey>>> column_comparators_;
  Status status_;
};

struct ResolvedRecordBatchSortKey {
  ResolvedRecordBatchSortKey(const std::shared_ptr<Array>& array, SortOrder order)
      : type(GetPhysicalType(array->type())),
        owned_array(GetPhysicalArray(*array, type)),
        array(*owned_array),
        order(order),
        null_count(array->null_count()) {}

  using LocationType = int64_t;

  ResolvedChunk GetChunk(int64_t index) const { return {&array, index}; }

  const std::shared_ptr<DataType> type;
  std::shared_ptr<Array> owned_array;
  const Array& array;
  SortOrder order;
  int64_t null_count;
};

struct ResolvedTableSortKey {
  ResolvedTableSortKey(const std::shared_ptr<DataType>& type, ArrayVector chunks,
                       SortOrder order, int64_t null_count)
      : type(GetPhysicalType(type)),
        owned_chunks(std::move(chunks)),
        chunks(GetArrayPointers(owned_chunks)),
        order(order),
        null_count(null_count) {}

  using LocationType = ::arrow::ChunkLocation;

  ResolvedChunk GetChunk(::arrow::ChunkLocation loc) const {
    return {chunks[loc.chunk_index], loc.index_in_chunk};
  }

  // Make a vector of ResolvedSortKeys for the sort keys and the given table.
  // `batches` must be a chunking of `table`.
  static Result<std::vector<ResolvedTableSortKey>> Make(
      const Table& table, const RecordBatchVector& batches,
      const std::vector<SortKey>& sort_keys) {
    auto factory = [&](const SortField& f) -> Result<ResolvedTableSortKey> {
      // We must expose a homogenous chunking for all ResolvedSortKey,
      // so we can't simply access the column from the table directly.
      ArrayVector chunks;
      chunks.reserve(batches.size());
      int64_t null_count = 0;
      for (const auto& batch : batches) {
        ARROW_ASSIGN_OR_RAISE(auto child, f.path.GetFlattened(*batch));
        null_count += child->null_count();
        chunks.push_back(std::move(child));
      }

      return ResolvedTableSortKey(f.type->GetSharedPtr(), std::move(chunks), f.order,
                                  null_count);
    };

    return ::arrow::compute::internal::ResolveSortKeys<ResolvedTableSortKey>(
        *table.schema(), sort_keys, factory);
  }

  std::shared_ptr<DataType> type;
  ArrayVector owned_chunks;
  std::vector<const Array*> chunks;
  SortOrder order;
  int64_t null_count;
};

inline Result<std::shared_ptr<ArrayData>> MakeMutableUInt64Array(
    int64_t length, MemoryPool* memory_pool) {
  auto buffer_size = length * sizeof(uint64_t);
  ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(buffer_size, memory_pool));
  return ArrayData::Make(uint64(), length, {nullptr, std::move(data)}, /*null_count=*/0);
}

inline Result<std::shared_ptr<ArrayData>> MakeMutableFloat64Array(
    int64_t length, MemoryPool* memory_pool) {
  auto buffer_size = length * sizeof(double);
  ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(buffer_size, memory_pool));
  return ArrayData::Make(float64(), length, {nullptr, std::move(data)}, /*null_count=*/0);
}

}  // namespace arrow::compute::internal
