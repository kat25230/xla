#pragma once

#include "torch_xla/csrc/ir.h"

namespace torch_xla {
namespace ir {
namespace ops {

class IndexGet : public Node {
 public:
  IndexGet(const ir::Value& base, const ir::Value& indices,
           xla::int64_t start_dim);

  std::string ToString() const override;

  NodePtr Clone(OpList operands) const override;

  XlaOpVector Lower(LoweringContext* loctx) const override;

  xla::int64_t start_dim() const { return start_dim_; }

 private:
  // The dimension number at which indexing starts.
  xla::int64_t start_dim_;
};

}  // namespace ops
}  // namespace ir
}  // namespace torch_xla
