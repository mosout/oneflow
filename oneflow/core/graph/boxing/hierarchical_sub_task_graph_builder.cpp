/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "oneflow/core/graph/boxing/hierarchical_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/chain_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/collective_boxing_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/slice_boxing_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/naive_b2b_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/naive_b2p_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/b21_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/one_to_one_sub_task_graph_builder.h"
#include "oneflow/core/graph/boxing/sub_task_graph_builder_util.h"

namespace oneflow {
namespace {

void ParallelAxesReduce(const ParallelDesc& parallel_desc,
                        const ParallelDistribution& parallel_distribution,
                        ParallelDesc* return_parallel_desc,
                        ParallelDistribution* return_parallel_distribution) {
  const auto& hierarchy = parallel_desc.hierarchy();
  DimVector reduced_hierarchy;
  reduced_hierarchy.push_back(hierarchy->At(0));
  *return_parallel_distribution->add_sbp_parallel() = parallel_distribution.sbp_parallel(0);
  FOR_RANGE(int64_t, i, 1, hierarchy->NumAxes()) {
    if (parallel_distribution.sbp_parallel(i) == parallel_distribution.sbp_parallel(i - 1)) {
      reduced_hierarchy.back() *= hierarchy->At(i);
    } else {
      reduced_hierarchy.push_back(hierarchy->At(i));
      *return_parallel_distribution->add_sbp_parallel() = parallel_distribution.sbp_parallel(i);
    }
  }
  ParallelConf reduced_parallel_conf = parallel_desc.parallel_conf();
  Shape(reduced_hierarchy).ToProto(reduced_parallel_conf.mutable_hierarchy());
  *return_parallel_desc = ParallelDesc(reduced_parallel_conf);
}

void CollaborativeParallelAxesReduce(const ParallelDesc& in_parallel_desc,
                                     const ParallelDesc& out_parallel_desc,
                                     const ParallelDistribution& in_parallel_distribution,
                                     const ParallelDistribution& out_parallel_distribution,
                                     ParallelDesc* in_return_parallel_desc,
                                     ParallelDesc* out_return_parallel_desc,
                                     ParallelDistribution* in_return_parallel_distribution,
                                     ParallelDistribution* out_return_parallel_distribution) {
  const auto& in_hierarchy = in_parallel_desc.hierarchy();
  const auto& out_hierarchy = out_parallel_desc.hierarchy();
  CHECK_EQ(in_hierarchy->NumAxes(), out_hierarchy->NumAxes());

  DimVector in_reduced_hierarchy;
  in_reduced_hierarchy.push_back(in_hierarchy->At(0));
  *in_return_parallel_distribution->add_sbp_parallel() = in_parallel_distribution.sbp_parallel(0);

  DimVector out_reduced_hierarchy;
  out_reduced_hierarchy.push_back(out_hierarchy->At(0));
  *out_return_parallel_distribution->add_sbp_parallel() = out_parallel_distribution.sbp_parallel(0);

  FOR_RANGE(int64_t, i, 1, in_hierarchy->NumAxes()) {
    if ((in_parallel_distribution.sbp_parallel(i) == in_parallel_distribution.sbp_parallel(i - 1))
        && (out_parallel_distribution.sbp_parallel(i)
            == out_parallel_distribution.sbp_parallel(i - 1))) {
      in_reduced_hierarchy.back() *= in_hierarchy->At(i);
      out_reduced_hierarchy.back() *= out_hierarchy->At(i);
    } else {
      in_reduced_hierarchy.push_back(in_hierarchy->At(i));
      *in_return_parallel_distribution->add_sbp_parallel() =
          in_parallel_distribution.sbp_parallel(i);

      out_reduced_hierarchy.push_back(out_hierarchy->At(i));
      *out_return_parallel_distribution->add_sbp_parallel() =
          out_parallel_distribution.sbp_parallel(i);
    }
  }

  ParallelConf in_reduced_parallel_conf = in_parallel_desc.parallel_conf();
  Shape(in_reduced_hierarchy).ToProto(in_reduced_parallel_conf.mutable_hierarchy());
  *in_return_parallel_desc = ParallelDesc(in_reduced_parallel_conf);

  ParallelConf out_reduced_parallel_conf = out_parallel_desc.parallel_conf();
  Shape(out_reduced_hierarchy).ToProto(out_reduced_parallel_conf.mutable_hierarchy());
  *out_return_parallel_desc = ParallelDesc(out_reduced_parallel_conf);
}

void InOutParallelAxesReduce(const ParallelDesc& in_parallel_desc,
                             const ParallelDesc& out_parallel_desc,
                             const ParallelDistribution& in_parallel_distribution,
                             const ParallelDistribution& out_parallel_distribution,
                             ParallelDesc* return_in_parallel_desc,
                             ParallelDesc* return_out_parallel_desc,
                             ParallelDistribution* return_in_parallel_distribution,
                             ParallelDistribution* return_out_parallel_distribution) {
  const int64_t in_hierarchy_axes = in_parallel_desc.hierarchy()->NumAxes();
  const int64_t out_hierarchy_axes = out_parallel_desc.hierarchy()->NumAxes();
  if (in_hierarchy_axes == 1 && out_hierarchy_axes == 1) {
    *return_in_parallel_desc = in_parallel_desc;
    *return_out_parallel_desc = out_parallel_desc;
    *return_in_parallel_distribution = in_parallel_distribution;
    *return_out_parallel_distribution = out_parallel_distribution;
  } else if (in_hierarchy_axes != out_hierarchy_axes) {
    ParallelAxesReduce(in_parallel_desc, in_parallel_distribution, return_in_parallel_desc,
                       return_in_parallel_distribution);
    ParallelAxesReduce(out_parallel_desc, out_parallel_distribution, return_out_parallel_desc,
                       return_out_parallel_distribution);
  } else {
    CollaborativeParallelAxesReduce(in_parallel_desc, out_parallel_desc, in_parallel_distribution,
                                    out_parallel_distribution, return_in_parallel_desc,
                                    return_out_parallel_desc, return_in_parallel_distribution,
                                    return_out_parallel_distribution);
  }
}

Maybe<SubTskGphBuilderStatus> Build1dParallelHierarchySubTskGph(
    SubTskGphBuilderCtx* ctx, const std::shared_ptr<SubTskGphBuilder> sub_tsk_gph_builder,
    const std::vector<TaskNode*>& sorted_in_tasks, std::vector<TaskNode*>* sorted_out_tasks,
    std::vector<std::vector<TaskNode*>>* sorted_ctrl_tasks, const ParallelDesc& in_parallel_desc,
    const ParallelDesc& out_parallel_desc, const LogicalBlobId& lbi,
    const BlobDesc& logical_blob_desc, const ParallelDistribution& in_parallel_distribution,
    const ParallelDistribution& out_parallel_distribution, const Shape& time_shape) {
  Maybe<SubTskGphBuilderStatus> boxing_builder_status = TRY(sub_tsk_gph_builder->Build(
      ctx, sorted_in_tasks, sorted_out_tasks, sorted_ctrl_tasks, in_parallel_desc,
      out_parallel_desc, lbi, logical_blob_desc, in_parallel_distribution.sbp_parallel(0),
      out_parallel_distribution.sbp_parallel(0), time_shape));
  return boxing_builder_status;
}

}  // namespace

HierarchicalSubTskGphBuilder::HierarchicalSubTskGphBuilder() {
  std::vector<std::shared_ptr<SubTskGphBuilder>> builders;
  builders.emplace_back(new OneToOneSubTskGphBuilder());
  builders.emplace_back(new B21SubTskGphBuilder());
  if (!Global<ResourceDesc, ForSession>::Get()->nccl_use_compute_stream()) {
    builders.emplace_back(new CollectiveBoxingSubTskGphBuilder());
  }
  builders.emplace_back(new SliceBoxingSubTskGphBuilder());
  builders.emplace_back(new NaiveB2BSubTskGphBuilder());
  builders.emplace_back(new NaiveB2PSubTskGphBuilder());
  sub_tsk_gph_builder_.reset(new ChainSubTskGphBuilder(builders));
}

Maybe<SubTskGphBuilderStatus> HierarchicalSubTskGphBuilder::Build(
    SubTskGphBuilderCtx* ctx, const std::vector<TaskNode*>& sorted_in_tasks,
    std::vector<TaskNode*>* sorted_out_tasks,
    std::vector<std::vector<TaskNode*>>* sorted_ctrl_tasks, const ParallelDesc& in_parallel_desc,
    const ParallelDesc& out_parallel_desc, const LogicalBlobId& lbi,
    const BlobDesc& logical_blob_desc, const ParallelDistribution& in_parallel_distribution,
    const ParallelDistribution& out_parallel_distribution, const Shape& time_shape) const {
  ParallelDesc reduced_in_parallel_desc = in_parallel_desc;
  ParallelDesc reduced_out_parallel_desc = out_parallel_desc;
  ParallelDistribution reduced_in_parallel_distribution;
  ParallelDistribution reduced_out_parallel_distribution;
  InOutParallelAxesReduce(in_parallel_desc, out_parallel_desc, in_parallel_distribution,
                          out_parallel_distribution, &reduced_in_parallel_desc,
                          &reduced_out_parallel_desc, &reduced_in_parallel_distribution,
                          &reduced_out_parallel_distribution);
  const auto& reduced_in_parallel_hierarchy = reduced_in_parallel_desc.hierarchy();
  const auto& reduced_out_parallel_hierarchy = reduced_out_parallel_desc.hierarchy();

  if (reduced_in_parallel_hierarchy->NumAxes() == 1
      && reduced_out_parallel_hierarchy->NumAxes() == 1) {
    return Build1dParallelHierarchySubTskGph(
        ctx, sub_tsk_gph_builder_, sorted_in_tasks, sorted_out_tasks, sorted_ctrl_tasks,
        in_parallel_desc, out_parallel_desc, lbi, logical_blob_desc,
        reduced_in_parallel_distribution, reduced_out_parallel_distribution, time_shape);
  } else {
    UNIMPLEMENTED();
  }
  return Error::BoxingNotSupportedError();
}

}  // namespace oneflow
