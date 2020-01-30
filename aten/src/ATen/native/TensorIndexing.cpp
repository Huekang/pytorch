#include <ATen/ExpandUtils.h>
#include <ATen/NativeFunctions.h>
#include <ATen/DeviceGuard.h>

#include <ATen/native/TensorIndexing.h>

#include <c10/util/Exception.h>

namespace at {
namespace indexing {

const EllipsisIndexType Ellipsis = EllipsisIndexType();

std::ostream& operator<<(std::ostream& stream, const Slice& slice) {
  stream << slice.start() << ":" << slice.stop() << ":" << slice.step();
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const TensorIndex& tensor_index) {
  if (tensor_index.is_none()) {
    stream << "None";
  } else if (tensor_index.is_ellipsis()) {
    stream << "...";
  } else if (tensor_index.is_integer()) {
    stream << tensor_index.integer();
  } else if (tensor_index.is_boolean()) {
    stream << std::boolalpha << tensor_index.boolean();
  } else if (tensor_index.is_slice()) {
    stream << tensor_index.slice();
  } else if (tensor_index.is_tensor()) {
    stream << tensor_index.tensor();
  }
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const std::vector<TensorIndex>& tensor_indices) {
  stream << "(";
  for (size_t i = 0; i < tensor_indices.size(); i++) {
    stream << tensor_indices[i];
    if (i < tensor_indices.size() - 1) stream << ", ";
  }
  stream << ")";
  return stream;
}

// This mirrors `count_specified_dimensions` in torch/csrc/autograd/python_variable_indexing.cpp
inline int64_t count_specified_dimensions(ArrayRef<TensorIndex> indices) {
  // Count the number of indexed dimensions (everything but ellipsis and None)
  int64_t count = 0;
  size_t size = indices.size();
  for (size_t i = 0; i < size; i++) {
    auto& obj = indices[i];
    if (obj.is_tensor()) {
      auto& tensor = obj.tensor();
      if (tensor.scalar_type() == kByte || tensor.scalar_type() == kBool) {
        count += tensor.dim();
      } else {
        count++;
      }
    } else if (!obj.is_none() && !obj.is_ellipsis() && !obj.is_boolean()) {
      count++;
    }
  }
  return count;
}

// This mirrors `valueToTensor` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor valueToTensor(c10::TensorOptions options, Scalar v) {
  return at::native::scalar_tensor(v, options);
}

// This mirrors `boolToIndexingTensor` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor boolToIndexingTensor(const Tensor& self, bool value) {
  // booleans add a dimension of size 1. true indexes this dimension as if 0:, false as empty.
  if (value) {
    return at::native::zeros({1}, {}, self.options().dtype(kLong));
  } else {
    return at::native::empty({0}, {}, self.options().dtype(kLong));
  }
}

// This mirrors `applySlicing` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor applySlicing(const Tensor& self, ArrayRef<TensorIndex> indices, std::vector<Tensor>& outIndices) {
  int64_t size = indices.size();
  int64_t dim = 0;
  int64_t specified_dims = count_specified_dimensions(indices);

  auto handle_tensor = [&](const Tensor& tensor) {
    // TODO: check scalarType
    outIndices.resize(dim + 1);
    outIndices[dim] = tensor;
    dim++;
  };

  TORCH_CHECK_INDEX(specified_dims <= self.dim(), "too many indices for tensor of dimension ", (int)self.dim());

  Tensor result = self;
  for (int64_t i = 0; i < size; i++) {
    auto& obj = indices[i];
    if (obj.is_integer()) {
      result = applySelect(
        result,
        dim,
        obj.integer(),
        obj.is_integer_with_tensor() ? obj.tensor() : Tensor(),
        i);
    } else if (obj.is_slice()) {
      result = applySlice(
        result,
        dim,
        obj.slice().start(),
        obj.slice().stop(),
        obj.slice().step(),
        obj.slice().start_tensor(),
        obj.slice().stop_tensor(),
        obj.slice().step_tensor());
      dim++;
    } else if (obj.is_ellipsis()) {
      dim += self.dim() - specified_dims;
    } else if (obj.is_none()) {
      result = result.unsqueeze(dim);
      dim++;
    } else if (obj.is_boolean()) {
      result = result.unsqueeze(dim);
      handle_tensor(boolToIndexingTensor(result, obj.boolean()));
    } else if (obj.is_tensor()) {
      auto& tensor = obj.tensor();
      auto scalar_type = tensor.scalar_type();
      if (tensor.dim() == 0 && at::isIntegralType(scalar_type, /*includeBool=*/true)) {
        if (scalar_type != at::kByte && scalar_type != at::kBool) {
          result = applySelect(result, dim, tensor.item<int64_t>(), tensor, i);
        } else {
          result = result.unsqueeze(dim);
          if(scalar_type == at::kBool) {
            handle_tensor(boolToIndexingTensor(result, tensor.item<bool>() != 0));
          } else {
            handle_tensor(boolToIndexingTensor(result, tensor.item<uint8_t>() != 0));
          }
        }
      } else {
        handle_tensor(tensor);
      }
    } else {
      TORCH_INTERNAL_ASSERT(false, "Invalid TensorIndex type");
    }
  }
  return result;
}

// This mirrors `typeConvertIndices` in torch/csrc/autograd/python_variable_indexing.cpp
inline std::vector<Tensor> typeConvertIndices(const Tensor& self, const std::vector<Tensor>& indices) {
  std::vector<Tensor> converted_inds(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    const auto &ind = indices[i];
    if (ind.defined()) {
      converted_inds[i] = ind.to(ind.options().device(self.device()));
    } else {
      converted_inds[i] = indices[i];
    }
  }
  return converted_inds;
}

// This mirrors `dispatch_index` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor dispatch_index(const Tensor& self, const std::vector<Tensor>& indices) {
  std::vector<Tensor> converted_indices = typeConvertIndices(self, indices);
  OptionalDeviceGuard device_guard(device_of(self));
  return self.index(converted_indices);
}

// This mirrors `dispatch_index_put_` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor dispatch_index_put_(Tensor& self, const std::vector<Tensor>& indices, const Tensor& value) {
  std::vector<Tensor> converted_indices = typeConvertIndices(self, indices);
  OptionalDeviceGuard device_guard(device_of(self));
  return self.index_put_(converted_indices, value);
}

// This mirrors `THPVariable_getitem` in torch/csrc/autograd/python_variable_indexing.cpp
inline Tensor get_item(const Tensor& self, ArrayRef<TensorIndex> indices) {
  OptionalDeviceGuard device_guard(device_of(self));

  // handle simple types: integers, slices, ellipsis
  if (indices.size() == 1) {
    const TensorIndex& index = indices[0];
    if (index.is_none()) {
      return self.unsqueeze(0);
    } else if (index.is_ellipsis()) {
      return self.alias();
    } else if (index.is_integer()) {
      return applySelect(self, 0, index.integer(), index.is_integer_with_tensor() ? index.tensor() : Tensor());
    } else if (index.is_slice()) {
      return applySlice(
        self,
        0,
        index.slice().start(),
        index.slice().stop(),
        index.slice().step(),
        index.slice().start_tensor(),
        index.slice().stop_tensor(),
        index.slice().step_tensor(),
        true);
    }
  }

  std::vector<Tensor> tensorIndices;
  Tensor sliced = applySlicing(self, indices, tensorIndices);
  if (tensorIndices.empty()) {
    if (sliced.is_same(self)) {
      // ensure we return a shallow copy for things like x[...]
      sliced = sliced.alias();
    }
    return sliced;
  }

  // indexing by tensors ("advanced" indexing)
  return dispatch_index(sliced, tensorIndices);
}

// This mirrors `slicePrefix1sSize` in torch/csrc/autograd/python_variable_indexing.cpp
//
// To match numpy semantics:
// As a special case for backwards compatibility,
// strip away unit dimensions from the left of 'src'
inline IntArrayRef slicePrefix1sSize(IntArrayRef sizes) {
  size_t first_non1_src = sizes.size();
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (sizes[i] != 1) {
      first_non1_src = i;
      break;
    }
  }

  return sizes.slice(first_non1_src);
}

// This mirrors `copy_to` in torch/csrc/autograd/python_variable_indexing.cpp
inline void copy_to(Tensor dst, const Tensor& src) {
  Tensor b_src;
  IntArrayRef sliced_src_sizes = slicePrefix1sSize(src.sizes());
  std::tie(b_src) = expand_inplace(dst, src.view(sliced_src_sizes), "setitem");
  dst.copy_(b_src);
}

// This mirrors `THPVariable_setitem` in torch/csrc/autograd/python_variable_indexing.cpp
// for "the assigned value is a Tensor" case
inline void set_item(Tensor& self, ArrayRef<TensorIndex> indices, const Tensor& value) {
  OptionalDeviceGuard device_guard(device_of(self));

  // handle simple types: integers, slices, ellipsis, bool
  if (indices.size() == 1) {
    const TensorIndex& index = indices[0];
    if (index.is_boolean() && !index.boolean()) {
      // do nothing for false (technically we should check the size, but we don't have
      // real 0-sized shapes.
      return;
    } else if (index.is_ellipsis()) {
      copy_to(self, value);
      return;
    } else if (index.is_none() || (index.is_boolean() && index.boolean())) {
      copy_to(self.unsqueeze(0), value);
      return;
    } else if (index.is_integer()) {
      copy_to(applySelect(self, 0, index.integer(), index.is_integer_with_tensor() ? index.tensor() : Tensor()), value);
      return;
    } else if (index.is_slice()) {
      copy_to(applySlice(
        self,
        0,
        index.slice().start(),
        index.slice().stop(),
        index.slice().step(),
        index.slice().start_tensor(),
        index.slice().stop_tensor(),
        index.slice().step_tensor()), value);
      return;
    }
  }

  std::vector<Tensor> tensorIndices;
  Tensor sliced = applySlicing(self, indices, tensorIndices);
  if (tensorIndices.empty()) {
    copy_to(sliced, value);
    return;
  }

  IntArrayRef slicedValueSizes = slicePrefix1sSize(value.sizes());
  Tensor valuesSliced;
  if (!value.sizes().equals(slicedValueSizes)) {
    valuesSliced = value.view(slicedValueSizes);
  } else {
    valuesSliced = value;
  }
  dispatch_index_put_(sliced, tensorIndices, valuesSliced);
  return;
}

// This mirrors `set_item` in torch/csrc/autograd/python_variable_indexing.cpp
// for "the assigned value is a Scalar" case
inline void set_item(Tensor& self, ArrayRef<TensorIndex> indices, Scalar v) {
  OptionalDeviceGuard device_guard(device_of(self));
  Tensor value;

  // TODO: This qint special case looks very suspicious...
  if (isQIntType(self.scalar_type())) {
    value = valueToTensor(device(kCPU).dtype(kFloat), v);
  } else {
    value = valueToTensor(self.options(), v);
  }

  return set_item(self, indices, value);
}

} // namespace indexing

Tensor Tensor::index(ArrayRef<TensorIndex> indices) const {
  return at::indexing::get_item(*this, indices);
}
Tensor Tensor::index(std::initializer_list<TensorIndex> indices) const {
  return index(ArrayRef<TensorIndex>(indices));
}

Tensor & Tensor::index_put_(ArrayRef<TensorIndex> indices, Tensor const & rhs) {
  at::indexing::set_item(*this, indices, rhs);
  return *this;
}
Tensor & Tensor::index_put_(ArrayRef<TensorIndex> indices, Tensor && rhs) {
  at::indexing::set_item(*this, indices, rhs);
  return *this;
}
Tensor & Tensor::index_put_(ArrayRef<TensorIndex> indices, Scalar v) {
  at::indexing::set_item(*this, indices, v);
  return *this;
}
Tensor & Tensor::index_put_(std::initializer_list<TensorIndex> indices, Tensor const & rhs) {
  return index_put_(ArrayRef<TensorIndex>(indices), rhs);
}
Tensor & Tensor::index_put_(std::initializer_list<TensorIndex> indices, Tensor && rhs) {
  return index_put_(ArrayRef<TensorIndex>(indices), rhs);
}
Tensor & Tensor::index_put_(std::initializer_list<TensorIndex> indices, Scalar v) {
  return index_put_(ArrayRef<TensorIndex>(indices), v);
}

} // namespace at
