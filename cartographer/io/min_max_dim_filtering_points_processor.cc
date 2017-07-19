/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/io/min_max_dim_filtering_points_processor.h"

#include "cartographer/common/lua_parameter_dictionary.h"
#include "cartographer/common/make_unique.h"
#include "cartographer/io/points_batch.h"

namespace cartographer {
namespace io {

std::unique_ptr<MinMaxDimFilteringPointsProcessor>
MinMaxDimFilteringPointsProcessor::FromDictionary(
    common::LuaParameterDictionary* const dictionary,
    PointsProcessor* const next) {
  return common::make_unique<MinMaxDimFilteringPointsProcessor>(
      dictionary->GetDouble("min"), dictionary->GetDouble("max"), dictionary->GetInt("dim"),
      next);
}

MinMaxDimFilteringPointsProcessor::MinMaxDimFilteringPointsProcessor(
    const double min, const double max, const size_t dim, PointsProcessor* next)
    : min_(min), max_(max), dim_(dim), next_(next) {}

void MinMaxDimFilteringPointsProcessor::Process(
    std::unique_ptr<PointsBatch> batch) {
  std::vector<int> to_remove;
  for (size_t i = 0; i < batch->points.size(); ++i) 
  {
    float dim_val = batch->points[i](dim_);
    if (dim_val < min_ || dim_val > max_) 
    {
      to_remove.push_back(i);
    }
  }
  RemovePoints(to_remove, batch.get());
  next_->Process(std::move(batch));
}

PointsProcessor::FlushResult MinMaxDimFilteringPointsProcessor::Flush() {
  return next_->Flush();
}

}  // namespace io
}  // namespace cartographer
