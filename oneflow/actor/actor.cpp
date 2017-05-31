#include "actor/actor.h"
#include "kernel/kernel_manager.h"

namespace oneflow {

void Actor::Init(const TaskProto& task_proto) {
  // actor_id
  actor_id_ = task_proto.id();
  // ward_func
  if (task_proto.is_forward()) {
    ward_func_ = &Kernel::Forward;
  } else {
    ward_func_ = &Kernel::Backward;
  }
  // exec_kernel_vec_
  exec_kernel_vec_.reserve(task_proto.exec_sequence().exec_node_size());
  for (const ExecNodeProto& node : task_proto.exec_sequence().exec_node()) {
    ExecKernel ek;
    ek.kernel = KernelMgr::Singleton().GetKernelFromOpName(node.op_name());
    ek.bn_in_op2regst_desc_id = PbMap2HashMap(node.bn_in_op2regst_desc_id());
    exec_kernel_vec_.push_back(std::move(ek));
  }
}

void Actor::WardKernel(std::function<Regst*(uint64_t)> GetRegstFromRegstDescId) {
  for (const ExecKernel& ek : exec_kernel_vec_) {
    (ek.kernel->*ward_func_)([&](const std::string& bn_in_op) {
      uint64_t regst_desc_id = ek.bn_in_op2regst_desc_id.at(bn_in_op);
      Regst* regst = GetRegstFromRegstDescId(regst_desc_id);
      const std::string& lbn = ek.kernel->GetLbnFromBnInOp(bn_in_op);
      return regst->GetBlobPtrFromLbn(lbn);
    });
  }
}

} // namespace oneflow
