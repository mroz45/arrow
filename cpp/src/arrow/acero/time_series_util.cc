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

#include "arrow/array/data.h"

#include "arrow/acero/time_series_util.h"
#include "arrow/util/logging.h"

namespace arrow::acero {

  template <typename T, enable_if_t<std::is_integral<T>::value, bool>>
  inline int64_t NormalizeTime(T t) {
    if constexpr(std::is_same_v<T, uint64_t>){
      return static_cast<int64_t>(t) ^ std::numeric_limits<int64_t>::lowest();
    } else{
      return t;
    }
  }

int64_t GetTime(const RecordBatch* batch, Type::type time_type, int col, uint64_t row) {
#define LATEST_VAL_CASE(id, val)                     \
  case Type::id: {                                   \
    using T = typename TypeIdTraits<Type::id>::Type; \
    using CType = typename TypeTraits<T>::CType;     \
    return val(data->GetValues<CType>(1)[row]);      \
  }

  auto data = batch->column_data(col);
  switch (time_type) {
    LATEST_VAL_CASE(INT8, NormalizeTime)
    LATEST_VAL_CASE(INT16, NormalizeTime)
    LATEST_VAL_CASE(INT32, NormalizeTime)
    LATEST_VAL_CASE(INT64, NormalizeTime)
    LATEST_VAL_CASE(UINT8, NormalizeTime)
    LATEST_VAL_CASE(UINT16, NormalizeTime)
    LATEST_VAL_CASE(UINT32, NormalizeTime)
    LATEST_VAL_CASE(UINT64, NormalizeTime)
    LATEST_VAL_CASE(DATE32, NormalizeTime)
    LATEST_VAL_CASE(DATE64, NormalizeTime)
    LATEST_VAL_CASE(TIME32, NormalizeTime)
    LATEST_VAL_CASE(TIME64, NormalizeTime)
    LATEST_VAL_CASE(TIMESTAMP, NormalizeTime)
    default:
      DCHECK(false);
      return 0;  // cannot happen
  }

#undef LATEST_VAL_CASE
}

}  // namespace arrow::acero
