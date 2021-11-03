#pragma once

#include <vector>

#include "torch_xla/csrc/ir.h"

namespace torch_xla {
namespace ir {
namespace ops {

class View : public Node {
 public:
  View(const Value& input, std::vector<xla::int64_t> output_size);

  XlaOpVector Lower(LoweringContext* loctx) const override;

  std::string ToString() const override;

  const std::vector<xla::int64_t>& output_size() const { return output_size_; }

 private:
  std::vector<xla::int64_t> output_size_;
};

}  // namespace ops
}  // namespace ir
}  // namespace torch_xla
