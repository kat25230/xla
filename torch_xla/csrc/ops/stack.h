#pragma once

#include "absl/types/span.h"
#include "torch_xla/csrc/ir.h"

namespace torch_xla {
namespace ir {
namespace ops {

class Stack : public Node {
 public:
  Stack(absl::Span<const ir::Value> values, xla::int64_t dim);

  std::string ToString() const override;

  NodePtr Clone(OpList operands) const override;

  XlaOpVector Lower(LoweringContext* loctx) const override;

  xla::int64_t dim() const { return dim_; };

 private:
  xla::int64_t dim_;
};

}  // namespace ops
}  // namespace ir
}  // namespace torch_xla
