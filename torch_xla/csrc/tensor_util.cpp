#include "torch_xla/csrc/tensor_util.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <list>
#include <numeric>
#include <thread>

#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "tensorflow/compiler/xla/xla_client/multi_wait.h"
#include "tensorflow/compiler/xla/xla_client/sys_util.h"
#include "tensorflow/compiler/xla/xla_client/tf_logging.h"
#include "tensorflow/compiler/xla/xla_client/thread_pool.h"
#include "tensorflow/compiler/xla/xla_client/util.h"
#include "tensorflow/core/lib/bfloat16/bfloat16.h"
#include "torch/csrc/lazy/core/hash.h"
#include "torch_xla/csrc/helpers.h"
#include "torch_xla/csrc/layout_manager.h"

namespace torch_xla {
namespace {

bool ShouldUseBF16() {
  bool use_bf16 = xla::sys_util::GetEnvBool("XLA_USE_BF16", false);
  if (use_bf16) {
    TF_LOG(INFO) << "Using BF16 data type for floating point values";
  }
  return use_bf16;
}

bool ShouldUseF16() {
  bool use_fp16 = xla::sys_util::GetEnvBool("XLA_USE_FP16", false);
  if (use_fp16) {
    TF_LOG(INFO) << "Using F16 data type for floating point values";
  }
  return use_fp16;
}

bool ShouldDowncastToBF16() {
  bool downcast_bf16 = xla::sys_util::GetEnvBool("XLA_DOWNCAST_BF16", false);
  if (downcast_bf16) {
    TF_LOG(INFO) << "Downcasting floating point values, F64->F32, F32->BF16";
  }
  return downcast_bf16;
}

bool ShouldDowncastToF16() {
  bool downcast_fp16 = xla::sys_util::GetEnvBool("XLA_DOWNCAST_FP16", false);
  if (downcast_fp16) {
    TF_LOG(INFO) << "Downcasting floating point values, F64->F32, F32->FP16";
  }
  return downcast_fp16;
}

bool ShouldUse32BitLong() {
  bool use_32bit_long = xla::sys_util::GetEnvBool("XLA_USE_32BIT_LONG", false);
  if (use_32bit_long) {
    TF_LOG(INFO) << "Using 32bit integers for kLong values";
  }
  return use_32bit_long;
}

bool UseBF16() {
  static bool use_bf16 = ShouldUseBF16();
  return use_bf16;
}

bool UseF16() {
  static bool use_fp16 = ShouldUseF16();
  return use_fp16;
}

bool DowncastBF16() {
  static bool downcast_bf16 = ShouldDowncastToBF16();
  return downcast_bf16;
}

bool DowncastF16() {
  static bool downcast_fp16 = ShouldDowncastToF16();
  return downcast_fp16;
}

bool Use32BitLong() {
  static bool use_32bit_long = ShouldUse32BitLong();
  return use_32bit_long;
}

xla::PrimitiveType XlaTypeFromTensorType(at::ScalarType scalar_type,
                                         const Device& device) {
  switch (scalar_type) {
    case at::ScalarType::Double:
      return device.hw_type != DeviceType::TPU ? xla::PrimitiveType::F64
                                               : xla::PrimitiveType::F32;
    case at::ScalarType::Float:
      return xla::PrimitiveType::F32;
    case at::ScalarType::BFloat16:
      return xla::PrimitiveType::BF16;
    case at::ScalarType::Half:
      return xla::PrimitiveType::F16;
    case at::ScalarType::Bool:
      return xla::PrimitiveType::PRED;
    case at::ScalarType::Byte:
      return xla::PrimitiveType::U8;
    case at::ScalarType::Char:
      return xla::PrimitiveType::S8;
    case at::ScalarType::Short:
      return xla::PrimitiveType::S16;
    case at::ScalarType::Int:
      return xla::PrimitiveType::S32;
    case at::ScalarType::Long:
      return xla::PrimitiveType::S64;
    case at::ScalarType::ComplexFloat:
      return xla::PrimitiveType::C64;
    case at::ScalarType::ComplexDouble:
      return xla::PrimitiveType::C128;
    default:
      XLA_ERROR() << "Type not supported: " << scalar_type;
  }
}

template <typename S>
struct Caster {
  template <typename D>
  D cast(const S& value) const {
    return static_cast<D>(value);
  }
};
template <>
struct Caster<at::BFloat16> {
  template <typename D>
  D cast(const at::BFloat16& value) const {
    return static_cast<D>(static_cast<float>(value));
  }
};
template <>
struct Caster<tensorflow::bfloat16> {
  template <typename D>
  D cast(const tensorflow::bfloat16& value) const {
    return static_cast<D>(static_cast<float>(value));
  }
};
template <>
struct Caster<at::Half> {
  template <typename D>
  D cast(const at::Half& value) const {
    return static_cast<D>(static_cast<float>(value));
  }
};
template <>
struct Caster<xla::half> {
  template <typename D>
  D cast(const xla::half& value) const {
    return static_cast<D>(static_cast<float>(value));
  }
};
template <>
struct Caster<c10::complex<float>> {
  template <typename D>
  D cast(const c10::complex<float>& value) const {
    return static_cast<D>(value.real());
  }
};

template <>
std::complex<float> Caster<c10::complex<float>>::cast<std::complex<float>>(
    const c10::complex<float>& value) const {
  return std::complex<float>(value.real(), value.imag());
}

template <>
std::complex<double> Caster<c10::complex<float>>::cast<std::complex<double>>(
    const c10::complex<float>& value) const {
  return std::complex<double>(value.real(), value.imag());
}

template <>
struct Caster<c10::complex<double>> {
  template <typename D>
  D cast(const c10::complex<double>& value) const {
    return static_cast<D>(value.real());
  }
};

template <>
std::complex<float> Caster<c10::complex<double>>::cast<std::complex<float>>(
    const c10::complex<double>& value) const {
  return std::complex<float>(value.real(), value.imag());
}

template <>
std::complex<double> Caster<c10::complex<double>>::cast<std::complex<double>>(
    const c10::complex<double>& value) const {
  return std::complex<double>(value.real(), value.imag());
}

template <>
struct Caster<std::complex<float>> {
  template <typename D>
  D cast(const std::complex<float>& value) const {
    return static_cast<D>(value.real());
  }
};

template <>
c10::complex<float> Caster<std::complex<float>>::cast<c10::complex<float>>(
    const std::complex<float>& value) const {
  return c10::complex<float>(value.real(), value.imag());
}
template <>
c10::complex<double> Caster<std::complex<float>>::cast<c10::complex<double>>(
    const std::complex<float>& value) const {
  return c10::complex<double>(value.real(), value.imag());
}

template <>
struct Caster<std::complex<double>> {
  template <typename D>
  D cast(const std::complex<double>& value) const {
    return static_cast<D>(value.real());
  }
};

// Copies n bytes from source to dest, with different stride values for source
// and destination.
template <typename S, typename D>
void StridedCopy(D* dest, xla::int64_t dest_stride, const S* source,
                 xla::int64_t source_stride, xla::int64_t n) {
  Caster<S> caster;
  const S* source_top = source + n * source_stride;
  for (; source < source_top; dest += dest_stride, source += source_stride) {
    *dest = caster.template cast<D>(*source);
  }
}

// Computes the offset of the value at a given index, assuming a contiguous/flat
// tensor data representation.
template <typename S>
xla::int64_t GetFlatTensorOffset(const S& strides,
                                 const std::vector<xla::int64_t>& indices) {
  xla::int64_t base = 0;
  for (size_t i = 0; i < indices.size(); ++i) {
    base += indices[i] * strides[i];
  }
  return base;
}

// The tensorflow::bfloat16 does not have implicit cast operations, so using
// std::copy() for it, is not going to work.
struct CopyDirect {};
struct CopyCasted {};

template <typename T>
struct NeedCast {
  static constexpr bool value = false;
};
template <>
struct NeedCast<tensorflow::bfloat16> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<at::BFloat16> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<xla::half> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<at::Half> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<c10::complex<float>> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<c10::complex<double>> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<std::complex<float>> {
  static constexpr bool value = true;
};
template <>
struct NeedCast<std::complex<double>> {
  static constexpr bool value = true;
};

template <bool CAST>
struct CopyType {
  using type = CopyDirect;
};
template <>
struct CopyType<true> {
  using type = CopyCasted;
};

template <typename D, typename S>
void CheckedMemcpy(D* dest, const S* source, xla::int64_t n) {
  static_assert(sizeof(S) == sizeof(D), "Types size mismatch");
  std::memcpy(dest, source, n * sizeof(S));
}

template <typename D, typename S>
void CopyData(D* dest, const S* source, xla::int64_t n, const CopyDirect&) {
  std::copy(source, source + n, dest);
}

template <typename D, typename S>
void CopyData(D* dest, const S* source, xla::int64_t n, const CopyCasted&) {
  // Use strided copy with step 1 since it has the static_cast<> required to
  // convert from/to bfloat16.
  StridedCopy(dest, 1, source, 1, n);
}

template <>
void CopyData<at::BFloat16, tensorflow::bfloat16>(
    at::BFloat16* dest, const tensorflow::bfloat16* source, xla::int64_t n,
    const CopyCasted&) {
  CheckedMemcpy<at::BFloat16, tensorflow::bfloat16>(dest, source, n);
}
template <>
void CopyData<tensorflow::bfloat16, at::BFloat16>(tensorflow::bfloat16* dest,
                                                  const at::BFloat16* source,
                                                  xla::int64_t n,
                                                  const CopyCasted&) {
  CheckedMemcpy<tensorflow::bfloat16, at::BFloat16>(dest, source, n);
}

std::vector<xla::int64_t> GetIterationDimensions(const xla::Shape& shape) {
  // We want to favor the most minor dimension as core iteration dimension, as
  // this walks one of the two tensors buffers in a cache friendly fashion.
  // Though, if the most minor dimension is too small, we will end up doing more
  // StridedCopy() iterations in CopyTensors().
  // So we select the most minor dimension, unless one of the other dimensions
  // is more than kMinorDimScale times the most minor one.
  static const xla::int64_t kMinorDimScale = 8;
  std::vector<xla::int64_t> iter_dims =
      xla::util::ToVector<xla::int64_t>(shape.layout().minor_to_major());
  size_t index = 0;
  xla::int64_t scaled_dim_size =
      kMinorDimScale * shape.dimensions(iter_dims[index]);
  for (size_t i = 1; i < iter_dims.size(); ++i) {
    xla::int64_t dim = iter_dims[i];
    if (shape.dimensions(dim) > scaled_dim_size) {
      index = i;
      scaled_dim_size = shape.dimensions(dim);
    }
  }
  std::swap(iter_dims[0], iter_dims[index]);
  return iter_dims;
}

struct CopyPartition {
  explicit CopyPartition(absl::Span<const xla::int64_t> dimensions)
      : base(dimensions.size()), limit(dimensions.begin(), dimensions.end()) {}

  std::vector<xla::int64_t> base;
  std::vector<xla::int64_t> limit;
};

std::vector<CopyPartition> CreateCopyPartitions(
    absl::Span<const xla::int64_t> dimensions,
    xla::int64_t strided_copy_dimension) {
  // The minimum number of elements copy that can be assigned to a thread.
  static const xla::int64_t kMinThreadElements = 100000;
  // Use at most 50% of the available cores.
  xla::int64_t max_parts =
      std::max<xla::int64_t>(std::thread::hardware_concurrency() / 2, 1);
  // Find the maximum dimension which is not the strided copy dimension.
  xla::int64_t max_dim = -1;
  for (xla::int64_t i = 0; i < dimensions.size(); ++i) {
    if (i != strided_copy_dimension &&
        (max_dim < 0 || dimensions[i] > dimensions[max_dim])) {
      max_dim = i;
    }
  }

  xla::int64_t num_elements = xla::util::Multiply<xla::int64_t>(dimensions);
  xla::int64_t max_dim_unit_elements = num_elements / dimensions[max_dim];
  xla::int64_t max_dim_size = dimensions[max_dim];
  xla::int64_t part_size = std::max<xla::int64_t>(
      std::max<xla::int64_t>(max_dim_size / max_parts, 1),
      kMinThreadElements / max_dim_unit_elements);
  std::vector<CopyPartition> parts;
  xla::int64_t csize = 0;
  while (csize < max_dim_size) {
    xla::int64_t n = std::min<xla::int64_t>(part_size, max_dim_size - csize);
    CopyPartition p(dimensions);
    p.base[max_dim] = csize;
    p.limit[max_dim] = csize + n;
    csize += n;
    parts.emplace_back(std::move(p));
  }
  return parts;
}

template <typename SType, typename DType>
void SlicedCopy(absl::Span<const xla::int64_t> dimensions,
                const SType* src_data,
                absl::Span<const xla::int64_t> src_strides, DType* dest_data,
                absl::Span<const xla::int64_t> dest_strides,
                absl::Span<const xla::int64_t> iter_dims,
                const CopyPartition& part) {
  std::vector<xla::int64_t> indices(part.base);
  xla::int64_t inner_src_stride = src_strides[iter_dims.front()];
  xla::int64_t inner_dest_stride = dest_strides[iter_dims.front()];
  xla::int64_t n = 0;
  while (n < indices.size()) {
    StridedCopy(dest_data + GetFlatTensorOffset(dest_strides, indices),
                inner_dest_stride,
                src_data + GetFlatTensorOffset(src_strides, indices),
                inner_src_stride, dimensions[iter_dims.front()]);
    for (n = 1; n < indices.size(); ++n) {
      xla::int64_t dim = iter_dims[n];
      indices[dim] += 1;
      if (indices[dim] < part.limit[dim]) {
        break;
      }
      indices[dim] = part.base[dim];
    }
  }
}

template <typename SType, typename DType>
void CopyTensors(const void* src_buffer, const xla::Shape& src_shape,
                 void* dest_buffer, size_t dest_buffer_size,
                 const xla::Shape& dest_shape) {
  XLA_CHECK(xla::ShapeUtil::SameDimensions(src_shape, dest_shape))
      << src_shape << " vs. " << dest_shape;

  xla::int64_t total_elements = xla::ShapeUtil::ElementsIn(src_shape);
  XLA_CHECK_EQ(dest_buffer_size, total_elements * sizeof(DType));

  const SType* src_data = reinterpret_cast<const SType*>(src_buffer);
  DType* dest_data = reinterpret_cast<DType*>(dest_buffer);
  if (src_shape.layout().minor_to_major() ==
      dest_shape.layout().minor_to_major()) {
    CopyData<DType, SType>(dest_data, src_data, total_elements,
                           typename CopyType < NeedCast<SType>::value ||
                               NeedCast<DType>::value > ::type());
  } else if (total_elements > 0) {
    // We issue a multi-threaded copy by slicing the bigger dimension and
    // assigning its copy to different threads. This code is only valid for
    // ranks >= 2, but the layout check above covers the case.
    std::vector<xla::int64_t> src_strides = ComputeShapeStrides(src_shape);
    std::vector<xla::int64_t> dest_strides = ComputeShapeStrides(dest_shape);
    std::vector<xla::int64_t> iter_dims = GetIterationDimensions(dest_shape);
    std::vector<CopyPartition> parts =
        CreateCopyPartitions(dest_shape.dimensions(), iter_dims.front());
    auto mwait = std::make_shared<xla::util::MultiWait>(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
      auto copy_fn = [&, i]() {
        SlicedCopy<SType, DType>(dest_shape.dimensions(), src_data, src_strides,
                                 dest_data, dest_strides, iter_dims, parts[i]);
      };
      xla::env::ScheduleClosure(
          xla::util::MultiWait::Completer(mwait, std::move(copy_fn)));
    }
    mwait->Wait();
  }
}

template <typename SType, typename DType>
void TensorToBuffer(const at::Tensor& tensor, const xla::Shape& dest_shape,
                    void* dest_buffer, size_t dest_buffer_size,
                    const Device& device) {
  at::Tensor contiguous_tensor = tensor.contiguous();
  xla::Shape src_shape = MakeTorchTensorLayout(
      XlaHelpers::I64List(contiguous_tensor.sizes()), /*dynamic_dimensions=*/{},
      XlaTypeFromTensorType(contiguous_tensor.type().scalarType(), device));
  CopyTensors<SType, DType>(contiguous_tensor.data_ptr<SType>(), src_shape,
                            dest_buffer, dest_buffer_size, dest_shape);
}

template <typename SType>
void TensorToBufferSType(const at::Tensor& tensor, const xla::Shape& dest_shape,
                         void* dest_buffer, size_t dest_buffer_size,
                         const Device& device) {
  switch (dest_shape.element_type()) {
    case xla::PrimitiveType::BF16:
      TensorToBuffer<SType, tensorflow::bfloat16>(
          tensor, dest_shape, dest_buffer, dest_buffer_size, device);
      break;
    case xla::PrimitiveType::F16:
      TensorToBuffer<SType, xla::half>(tensor, dest_shape, dest_buffer,
                                       dest_buffer_size, device);
      break;
    case xla::PrimitiveType::F32:
      TensorToBuffer<SType, float>(tensor, dest_shape, dest_buffer,
                                   dest_buffer_size, device);
      break;
    case xla::PrimitiveType::F64:
      TensorToBuffer<SType, double>(tensor, dest_shape, dest_buffer,
                                    dest_buffer_size, device);
      break;
    case xla::PrimitiveType::PRED:
      TensorToBuffer<SType, bool>(tensor, dest_shape, dest_buffer,
                                  dest_buffer_size, device);
      break;
    case xla::PrimitiveType::U8:
      TensorToBuffer<SType, xla::uint8>(tensor, dest_shape, dest_buffer,
                                        dest_buffer_size, device);
      break;
    case xla::PrimitiveType::S8:
      TensorToBuffer<SType, xla::int8>(tensor, dest_shape, dest_buffer,
                                       dest_buffer_size, device);
      break;
    case xla::PrimitiveType::S16:
      TensorToBuffer<SType, xla::int16>(tensor, dest_shape, dest_buffer,
                                        dest_buffer_size, device);
      break;
    case xla::PrimitiveType::U16:
      TensorToBuffer<SType, xla::uint16>(tensor, dest_shape, dest_buffer,
                                         dest_buffer_size, device);
      break;
    case xla::PrimitiveType::S32:
      TensorToBuffer<SType, xla::int32>(tensor, dest_shape, dest_buffer,
                                        dest_buffer_size, device);
      break;
    case xla::PrimitiveType::U32:
      TensorToBuffer<SType, xla::uint32>(tensor, dest_shape, dest_buffer,
                                         dest_buffer_size, device);
      break;
    case xla::PrimitiveType::S64:
      TensorToBuffer<SType, xla::int64_t>(tensor, dest_shape, dest_buffer,
                                          dest_buffer_size, device);
      break;
    case xla::PrimitiveType::U64:
      TensorToBuffer<SType, xla::uint64>(tensor, dest_shape, dest_buffer,
                                         dest_buffer_size, device);
      break;
    case xla::PrimitiveType::C64:
      TensorToBuffer<SType, xla::complex64>(tensor, dest_shape, dest_buffer,
                                            dest_buffer_size, device);
      break;
    case xla::PrimitiveType::C128:
      TensorToBuffer<SType, xla::complex128>(tensor, dest_shape, dest_buffer,
                                             dest_buffer_size, device);
      break;
    default:
      XLA_ERROR() << "Destination shape type not supported: " << dest_shape;
  }
}

void PopulateTensorBuffer(const at::Tensor& tensor,
                          const xla::Shape& dest_shape, void* dest_buffer,
                          size_t dest_buffer_size, const Device& device) {
  switch (tensor.type().scalarType()) {
    case at::ScalarType::Double:
      TensorToBufferSType<double>(tensor, dest_shape, dest_buffer,
                                  dest_buffer_size, device);
      break;
    case at::ScalarType::Float:
      TensorToBufferSType<float>(tensor, dest_shape, dest_buffer,
                                 dest_buffer_size, device);
      break;
    case at::ScalarType::BFloat16:
      TensorToBufferSType<at::BFloat16>(tensor, dest_shape, dest_buffer,
                                        dest_buffer_size, device);
      break;
    case at::ScalarType::Half:
      TensorToBufferSType<at::Half>(tensor, dest_shape, dest_buffer,
                                    dest_buffer_size, device);
      break;
    case at::ScalarType::Bool:
      TensorToBufferSType<bool>(tensor, dest_shape, dest_buffer,
                                dest_buffer_size, device);
      break;
    case at::ScalarType::Byte:
      TensorToBufferSType<uint8_t>(tensor, dest_shape, dest_buffer,
                                   dest_buffer_size, device);
      break;
    case at::ScalarType::Char:
      TensorToBufferSType<int8_t>(tensor, dest_shape, dest_buffer,
                                  dest_buffer_size, device);
      break;
    case at::ScalarType::Short:
      TensorToBufferSType<int16_t>(tensor, dest_shape, dest_buffer,
                                   dest_buffer_size, device);
      break;
    case at::ScalarType::Int:
      TensorToBufferSType<int32_t>(tensor, dest_shape, dest_buffer,
                                   dest_buffer_size, device);
      break;
    case at::ScalarType::Long:
      TensorToBufferSType<int64_t>(tensor, dest_shape, dest_buffer,
                                   dest_buffer_size, device);
      break;
    case at::ScalarType::ComplexFloat:
      TensorToBufferSType<c10::complex<float>>(tensor, dest_shape, dest_buffer,
                                               dest_buffer_size, device);
      break;
    case at::ScalarType::ComplexDouble:
      TensorToBufferSType<c10::complex<double>>(tensor, dest_shape, dest_buffer,
                                                dest_buffer_size, device);
      break;
    default:
      XLA_ERROR() << "Tensor type not supported: " << tensor.type();
  }
}

xla::ComputationClient::DataPtr TensorToXlaData(const at::Tensor& tensor,
                                                const xla::Shape& shape,
                                                const Device& device) {
  auto populate_fn =
      [&](const xla::ComputationClient::TensorSource& source_tensor,
          void* dest_buffer, size_t dest_buffer_size) {
        PopulateTensorBuffer(tensor, source_tensor.shape, dest_buffer,
                             dest_buffer_size, device);
      };

  std::vector<xla::ComputationClient::TensorSource> source_tensors;
  source_tensors.emplace_back(shape, device.ToString(), std::move(populate_fn));

  auto handles =
      xla::ComputationClient::Get()->TransferToServer(source_tensors);
  XLA_CHECK_EQ(handles.size(), 1);
  return std::move(handles.front());
}

template <typename SType, typename DType>
at::Tensor XlaLiteralToTensor(const xla::Literal& literal,
                              at::ScalarType atype) {
  std::vector<int64_t> dimensions =
      xla::util::ToVector<int64_t>(literal.shape().dimensions());
  xla::Shape torch_shape = MakeTorchTensorLayout(
      literal.shape().dimensions(), /*dynamic_dimensions=*/{},
      literal.shape().element_type());
  xla::int64_t total_elements = xla::ShapeUtil::ElementsIn(torch_shape);

  const auto literal_data = literal.data<SType>();
  at::Tensor tensor = at::empty(dimensions, at::TensorOptions(atype));
  CopyTensors<SType, DType>(literal_data.data(), literal.shape(),
                            tensor.data_ptr<DType>(),
                            total_elements * sizeof(DType), torch_shape);
  return tensor;
}

template <typename SType>
at::Tensor XlaLiteralToTensorHelper(const xla::Literal& literal,
                                    at::ScalarType dest_element_type) {
  switch (dest_element_type) {
    case at::ScalarType::Bool:
      return XlaLiteralToTensor<SType, bool>(literal, dest_element_type);
    case at::ScalarType::Byte:
      return XlaLiteralToTensor<SType, uint8_t>(literal, dest_element_type);
    case at::ScalarType::Char:
      return XlaLiteralToTensor<SType, int8_t>(literal, dest_element_type);
    case at::ScalarType::Short:
      return XlaLiteralToTensor<SType, int16_t>(literal, dest_element_type);
    case at::ScalarType::Int:
      return XlaLiteralToTensor<SType, int32_t>(literal, dest_element_type);
    case at::ScalarType::Long:
      return XlaLiteralToTensor<SType, int64_t>(literal, dest_element_type);
    case at::ScalarType::Float:
      return XlaLiteralToTensor<SType, float>(literal, dest_element_type);
    case at::ScalarType::Double:
      return XlaLiteralToTensor<SType, double>(literal, dest_element_type);
    case at::ScalarType::BFloat16:
      return XlaLiteralToTensor<SType, at::BFloat16>(literal,
                                                     dest_element_type);
    case at::ScalarType::Half:
      return XlaLiteralToTensor<SType, at::Half>(literal, dest_element_type);
    case at::ScalarType::ComplexFloat:
      return XlaLiteralToTensor<SType, c10::complex<float>>(literal,
                                                            dest_element_type);
    case at::ScalarType::ComplexDouble:
      return XlaLiteralToTensor<SType, c10::complex<double>>(literal,
                                                             dest_element_type);
    default:
      XLA_ERROR() << "Unsupported scalar type: " << dest_element_type;
  }
}

}  // namespace

std::vector<xla::int64_t> ComputeShapeStrides(const xla::Shape& shape) {
  std::vector<xla::int64_t> strides(shape.rank());
  xla::int64_t stride = 1;
  for (auto dim : shape.layout().minor_to_major()) {
    strides[dim] = stride;
    stride *= shape.dimensions(dim);
  }
  return strides;
}

std::vector<xla::int64_t> ComputeArrayStrides(
    absl::Span<const xla::int64_t> sizes) {
  std::vector<xla::int64_t> strides(sizes.size(), 1);
  for (xla::int64_t i = sizes.size(); i > 1; --i) {
    strides[i - 2] = strides[i - 1] * sizes[i - 1];
  }
  return strides;
}

at::Tensor MakeTensorFromXlaLiteral(const xla::Literal& literal,
                                    at::ScalarType dest_element_type) {
  switch (literal.shape().element_type()) {
    case xla::PrimitiveType::PRED:
      return XlaLiteralToTensorHelper<bool>(literal, dest_element_type);
    case xla::PrimitiveType::BF16:
      return XlaLiteralToTensorHelper<tensorflow::bfloat16>(literal,
                                                            dest_element_type);
    case xla::PrimitiveType::F16:
      return XlaLiteralToTensorHelper<xla::half>(literal, dest_element_type);
    case xla::PrimitiveType::F32:
      return XlaLiteralToTensorHelper<float>(literal, dest_element_type);
    case xla::PrimitiveType::F64:
      return XlaLiteralToTensorHelper<double>(literal, dest_element_type);
    case xla::PrimitiveType::U8:
      return XlaLiteralToTensorHelper<xla::uint8>(literal, dest_element_type);
    case xla::PrimitiveType::S8:
      return XlaLiteralToTensorHelper<xla::int8>(literal, dest_element_type);
    case xla::PrimitiveType::S16:
      return XlaLiteralToTensorHelper<xla::int16>(literal, dest_element_type);
    case xla::PrimitiveType::U16:
      return XlaLiteralToTensorHelper<xla::uint16>(literal, dest_element_type);
    case xla::PrimitiveType::S32:
      return XlaLiteralToTensorHelper<xla::int32>(literal, dest_element_type);
    case xla::PrimitiveType::U32:
      return XlaLiteralToTensorHelper<xla::uint32>(literal, dest_element_type);
    case xla::PrimitiveType::S64:
      return XlaLiteralToTensorHelper<xla::int64_t>(literal, dest_element_type);
    case xla::PrimitiveType::U64:
      return XlaLiteralToTensorHelper<xla::uint64>(literal, dest_element_type);
    case xla::PrimitiveType::C64:
      return XlaLiteralToTensorHelper<xla::complex64>(literal,
                                                      dest_element_type);
    case xla::PrimitiveType::C128:
      return XlaLiteralToTensorHelper<xla::complex128>(literal,
                                                       dest_element_type);
    default:
      XLA_ERROR() << "Unsupported literal type: " << literal.shape();
  }
}

bool TensorCompare(const at::Tensor& t1, const at::Tensor& t2) {
  if (t1.scalar_type() != t2.scalar_type() || t1.sizes() != t2.sizes()) {
    return false;
  }
  // PyTorch currently has an issue comparing tensors which have NaN values in
  // it. The compare is not deterministic. So we do memory compare here until
  // the PyTorch equal() API is fixed.
  at::Tensor contiguous_t1 = t1.contiguous();
  at::Tensor contiguous_t2 = t2.contiguous();
  return std::memcmp(contiguous_t1.data_ptr(), contiguous_t2.data_ptr(),
                     contiguous_t1.numel() * contiguous_t1.itemsize()) == 0;
}

xla::ComputationClient::DataPtr TensorToXlaData(const at::Tensor& tensor,
                                                const Device& device) {
  return TensorToXlaData(
      tensor, CreateComputationShapeFromTensor(tensor, &device), device);
}

std::vector<xla::ComputationClient::DataPtr> CreateTensorsData(
    const std::vector<at::Tensor>& tensors,
    const std::vector<std::string>& devices) {
  XLA_CHECK_EQ(tensors.size(), devices.size());
  std::vector<xla::ComputationClient::TensorSource> source_tensors;
  for (size_t i = 0; i < tensors.size(); ++i) {
    Device device(devices[i]);
    xla::Shape shape = CreateComputationShapeFromTensor(tensors[i], &device);
    auto populate_fn =
        [&, i, device](
            const xla::ComputationClient::TensorSource& source_tensor,
            void* dest_buffer, size_t dest_buffer_size) {
          PopulateTensorBuffer(tensors[i], source_tensor.shape, dest_buffer,
                               dest_buffer_size, device);
        };
    source_tensors.emplace_back(std::move(shape), devices[i],
                                std::move(populate_fn));
  }
  return xla::ComputationClient::Get()->TransferToServer(source_tensors);
}

xla::Literal GetTensorLiteral(const at::Tensor& tensor, const xla::Shape* shape,
                              const Device* device) {
  Device xla_device = GetDeviceOrCurrent(device);
  xla::Shape computed_shape;
  if (shape == nullptr) {
    auto dimensions = XlaHelpers::I64List(tensor.sizes());
    computed_shape = MakeTorchTensorLayout(
        dimensions, /*dynamic_dimensions=*/{},
        XlaTypeFromTensorType(tensor.type().scalarType(), xla_device));
    shape = &computed_shape;
  }
  xla::Literal literal(*shape);
  PopulateTensorBuffer(tensor, *shape, literal.untyped_data(),
                       literal.size_bytes(), xla_device);
  return literal;
}

std::vector<at::Tensor> XlaDataToTensors(
    absl::Span<const xla::ComputationClient::DataPtr> xla_data,
    at::ScalarType dest_element_type) {
  std::vector<xla::Literal> literals =
      xla::ComputationClient::Get()->TransferFromServer(xla_data);
  std::vector<at::Tensor> tensors;
  tensors.reserve(literals.size());
  for (auto& literal : literals) {
    tensors.push_back(MakeTensorFromXlaLiteral(literal, dest_element_type));
  }
  return tensors;
}

torch::lazy::hash_t TensorHash(const at::Tensor& tensor) {
  at::Tensor ctensor = tensor.contiguous();
  int64_t size = ctensor.numel() * ctensor.element_size();
  switch (ctensor.scalar_type()) {
    case at::ScalarType::Bool:
      return torch::lazy::DataHash(ctensor.data_ptr<bool>(), size);
    case at::ScalarType::Byte:
      return torch::lazy::DataHash(ctensor.data_ptr<uint8_t>(), size);
    case at::ScalarType::Char:
      return torch::lazy::DataHash(ctensor.data_ptr<int8_t>(), size);
    case at::ScalarType::Short:
      return torch::lazy::DataHash(ctensor.data_ptr<int16_t>(), size);
    case at::ScalarType::Int:
      return torch::lazy::DataHash(ctensor.data_ptr<int32_t>(), size);
    case at::ScalarType::Long:
      return torch::lazy::DataHash(ctensor.data_ptr<int64_t>(), size);
    case at::ScalarType::Float:
      return torch::lazy::DataHash(ctensor.data_ptr<float>(), size);
    case at::ScalarType::Double:
      return torch::lazy::DataHash(ctensor.data_ptr<double>(), size);
    case at::ScalarType::BFloat16:
      return torch::lazy::DataHash(ctensor.data_ptr<at::BFloat16>(), size);
    case at::ScalarType::Half:
      return torch::lazy::DataHash(ctensor.data_ptr<at::Half>(), size);
    case at::ScalarType::ComplexFloat:
      return torch::lazy::DataHash(ctensor.data_ptr<c10::complex<float>>(),
                                   size);
    case at::ScalarType::ComplexDouble:
      return torch::lazy::DataHash(ctensor.data_ptr<c10::complex<double>>(),
                                   size);
    default:
      XLA_ERROR() << "Unsupported scalar type: " << ctensor.scalar_type();
  }
}

std::vector<xla::Shape> GetComponentShapes(const xla::Shape& shape) {
  std::vector<xla::Shape> component_shapes;
  if (shape.IsTuple()) {
    for (const xla::Shape& component_shape : shape.tuple_shapes()) {
      XLA_CHECK(!component_shape.IsTuple()) << shape;
      component_shapes.push_back(component_shape);
    }
  } else {
    component_shapes.push_back(shape);
  }
  return component_shapes;
}

xla::Shape MakeShapeWithDeviceLayout(const xla::Shape& shape,
                                     DeviceType device_type) {
  xla::Shape device_shape(shape);
  xla::ShapeUtil::ForEachMutableSubshape(
      &device_shape, [&](xla::Shape* subshape, const xla::ShapeIndex&) {
        if (subshape->IsArray()) {
          *subshape = MakeArrayShapeFromDimensions(
              subshape->dimensions(), subshape->dynamic_dimensions(),
              subshape->element_type(), device_type);
        }
      });
  return device_shape;
}

xla::Shape CreateComputationShapeFromTensor(const at::Tensor& tensor,
                                            const Device* device) {
  Device xla_device = GetDeviceOrCurrent(device);
  return MakeArrayShapeFromDimensions(
      XlaHelpers::I64List(tensor.sizes()),
      /*dynamic_dimensions=*/{},
      MakeXlaPrimitiveType(tensor.type().scalarType(), &xla_device),
      xla_device.hw_type);
}

at::ScalarType TensorTypeFromXlaType(xla::PrimitiveType xla_type) {
  switch (xla_type) {
    case xla::PrimitiveType::BF16:
      return UseBF16() || DowncastBF16() ? at::ScalarType::Float
                                         : at::ScalarType::BFloat16;
    case xla::PrimitiveType::F16:
      return UseF16() || DowncastF16() ? at::ScalarType::Float
                                       : at::ScalarType::Half;
    case xla::PrimitiveType::F32:
      return DowncastBF16() || DowncastF16() ? at::ScalarType::Double
                                             : at::ScalarType::Float;
    case xla::PrimitiveType::F64:
      return at::ScalarType::Double;
    case xla::PrimitiveType::PRED:
      return at::ScalarType::Bool;
    case xla::PrimitiveType::U8:
      return at::ScalarType::Byte;
    case xla::PrimitiveType::S8:
      return at::ScalarType::Char;
    case xla::PrimitiveType::S16:
    case xla::PrimitiveType::U16:
      return at::ScalarType::Short;
    case xla::PrimitiveType::S32:
    case xla::PrimitiveType::U32:
      return at::ScalarType::Int;
    case xla::PrimitiveType::S64:
    case xla::PrimitiveType::U64:
      return at::ScalarType::Long;
    case xla::PrimitiveType::C64:
      return at::ScalarType::ComplexFloat;
    case xla::PrimitiveType::C128:
      return at::ScalarType::ComplexDouble;
    default:
      XLA_ERROR() << "XLA type not supported: " << xla_type;
  }
}

xla::PrimitiveType TensorTypeToRawXlaType(at::ScalarType scalar_type) {
  switch (scalar_type) {
    case at::ScalarType::Double:
      return xla::PrimitiveType::F64;
    case at::ScalarType::Float:
      return xla::PrimitiveType::F32;
    case at::ScalarType::BFloat16:
      return xla::PrimitiveType::BF16;
    case at::ScalarType::Half:
      return xla::PrimitiveType::F16;
    case at::ScalarType::Bool:
      return xla::PrimitiveType::PRED;
    case at::ScalarType::Byte:
      return xla::PrimitiveType::U8;
    case at::ScalarType::Char:
      return xla::PrimitiveType::S8;
    case at::ScalarType::Short:
      return xla::PrimitiveType::S16;
    case at::ScalarType::Int:
      return xla::PrimitiveType::S32;
    case at::ScalarType::Long:
      return xla::PrimitiveType::S64;
    case at::ScalarType::ComplexFloat:
      return xla::PrimitiveType::C64;
    case at::ScalarType::ComplexDouble:
      return xla::PrimitiveType::C128;
    default:
      XLA_ERROR() << "Type not supported: " << scalar_type;
  }
}

xla::PrimitiveType GetDevicePrimitiveType(xla::PrimitiveType type,
                                          const Device* device) {
  Device xla_device = GetDeviceOrCurrent(device);
  switch (type) {
    case xla::PrimitiveType::F64:
      if (UseF16()) {
        return xla::PrimitiveType::F16;
      }
      if (UseBF16()) {
        return xla::PrimitiveType::BF16;
      }
      if (DowncastBF16() || DowncastF16()) {
        return xla::PrimitiveType::F32;
      }
      return xla_device.hw_type != DeviceType::TPU ? xla::PrimitiveType::F64
                                                   : xla::PrimitiveType::F32;
    case xla::PrimitiveType::F32:
      if (UseF16() || DowncastF16()) {
        return xla::PrimitiveType::F16;
      }
      return UseBF16() || DowncastBF16() ? xla::PrimitiveType::BF16
                                         : xla::PrimitiveType::F32;
    case xla::PrimitiveType::U16:
      return xla_device.hw_type != DeviceType::TPU ? xla::PrimitiveType::U16
                                                   : xla::PrimitiveType::U32;
    case xla::PrimitiveType::S16:
      return xla_device.hw_type != DeviceType::TPU ? xla::PrimitiveType::S16
                                                   : xla::PrimitiveType::S32;
    case xla::PrimitiveType::S64:
      return Use32BitLong() ? xla::PrimitiveType::S32 : xla::PrimitiveType::S64;
    case xla::PrimitiveType::U64:
      return Use32BitLong() ? xla::PrimitiveType::U32 : xla::PrimitiveType::U64;
    case xla::PrimitiveType::C128:
      return xla_device.hw_type != DeviceType::TPU ? xla::PrimitiveType::C128
                                                   : xla::PrimitiveType::C64;
    default:
      return type;
  }
}

xla::PrimitiveType MakeXlaPrimitiveType(at::ScalarType scalar_type,
                                        const Device* device) {
  switch (scalar_type) {
    case at::ScalarType::Double:
      return GetDevicePrimitiveType(xla::PrimitiveType::F64, device);
    case at::ScalarType::Float:
      return GetDevicePrimitiveType(xla::PrimitiveType::F32, device);
    case at::ScalarType::BFloat16:
      return GetDevicePrimitiveType(xla::PrimitiveType::BF16, device);
    case at::ScalarType::Half:
      return GetDevicePrimitiveType(xla::PrimitiveType::F16, device);
    case at::ScalarType::Bool:
      return GetDevicePrimitiveType(xla::PrimitiveType::PRED, device);
    case at::ScalarType::Byte:
      return GetDevicePrimitiveType(xla::PrimitiveType::U8, device);
    case at::ScalarType::Char:
      return GetDevicePrimitiveType(xla::PrimitiveType::S8, device);
    case at::ScalarType::Short:
      return GetDevicePrimitiveType(xla::PrimitiveType::S16, device);
    case at::ScalarType::Int:
      return GetDevicePrimitiveType(xla::PrimitiveType::S32, device);
    case at::ScalarType::Long:
      return GetDevicePrimitiveType(xla::PrimitiveType::S64, device);
    case at::ScalarType::ComplexFloat:
      return GetDevicePrimitiveType(xla::PrimitiveType::C64, device);
    case at::ScalarType::ComplexDouble:
      return GetDevicePrimitiveType(xla::PrimitiveType::C128, device);
    default:
      XLA_ERROR() << "Type not supported: " << scalar_type;
  }
}

bool RequiresRawTypeCasting(at::ScalarType scalar_type, const Device* device) {
  switch (scalar_type) {
    case at::ScalarType::Byte:
    case at::ScalarType::Char:
    case at::ScalarType::Short:
      return MakeXlaPrimitiveType(scalar_type, device) !=
             TensorTypeToRawXlaType(scalar_type);
    default:
      return false;
  }
}

xla::PrimitiveType GetShapeDimensionType(const Device* device) {
  Device xla_device = GetDeviceOrCurrent(device);
  return xla_device.hw_type == DeviceType::CPU ? xla::PrimitiveType::S64
                                               : xla::PrimitiveType::S32;
}

}  // namespace torch_xla
