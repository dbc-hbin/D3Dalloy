#pragma once
#include "nvngx.hpp"
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace dxmt {

using Parameter =
    std::variant<unsigned long long, float, double, unsigned int, int, void *, ID3D11Resource *, ID3D12Resource *>;

template <class... Ts> struct overloads : Ts... {
  using Ts::operator()...;
};

template <typename From, typename To>
inline bool
castable(From val, To *out) {
  return false;
}

template <>
inline bool
castable(int val, unsigned int *out) {
  *out = (unsigned int)val;
  return true;
}

template <>
inline bool
castable(unsigned int val, int *out) {
  *out = (int)val;
  return true;
}

template <>
inline bool
castable(unsigned int val, float *out) {
  *out = static_cast<float>(val);
  return true;
}

// Float-to-integer conversion is undefined outside the target range. NaN and
// out-of-range values are type mismatches, not truncated garbage.
template <>
inline bool
castable(float val, unsigned int *out) {
  if (!(val > -1.0f && val < 4294967296.0f))
    return false;
  *out = static_cast<unsigned int>(val);
  return true;
}

template <>
inline bool
castable(int val, float *out) {
  *out = static_cast<float>(val);
  return true;
}

template <>
inline bool
castable(float val, int *out) {
  if (!(val > -2147483904.0f && val < 2147483648.0f))
    return false;
  *out = static_cast<int>(val);
  return true;
}

template <>
inline bool
castable(double val, float *out) {
  *out = (float)(val);
  return true;
}

template <>
inline bool
castable(float val, double *out) {
  *out = (double)(val);
  return true;
}

template <>
inline bool
castable(unsigned long long val, void **out) {
  *out = (void *)val;
  return true;
}

// Streamline stores D3D12 resources through the void* overload. Preserve the
// pointer alias without accepting numeric values or D3D11 resources.
template <>
inline bool
castable(void *val, ID3D12Resource **out) {
  *out = static_cast<ID3D12Resource *>(val);
  return true;
}

template <>
inline bool
castable(ID3D12Resource *val, void **out) {
  *out = val;
  return true;
}

template <>
inline bool
castable(void *val, unsigned long long *out) {
  *out = (unsigned long long)val;
  return true;
}

template <>
inline bool
castable(unsigned long long val, int *out) {
  *out = (int)val;
  return true;
}

template <>
inline bool
castable(int val, unsigned long long *out) {
  *out = (unsigned long long)val;
  return true;
}

template <>
inline bool
castable(unsigned long long val, unsigned int *out) {
  *out = (int)val;
  return true;
}

template <>
inline bool
castable(unsigned int val, unsigned long long *out) {
  *out = (unsigned long long)val;
  return true;
}

class ParametersImpl final : public NVNGXParameter {
public:
  virtual void
  Set(const char *name, unsigned long long value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, float value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, double value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, unsigned int value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, int value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, ID3D11Resource *value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, ID3D12Resource *value) final {
    SetType(name, value);
  }
  virtual void
  Set(const char *name, void *value) final {
    SetType(name, value);
  }

  virtual NVNGX_RESULT
  Get(const char *name, unsigned long long *out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, float *out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, double *out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, unsigned int *out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, int *out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, ID3D11Resource **out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, ID3D12Resource **out) const final {
    return GetType(name, out);
  }
  virtual NVNGX_RESULT
  Get(const char *name, void **out) const final {
    return GetType<void *>(name, out);
  }

  virtual void
  Reset() final {
    values_.clear();
  }

private:
  // Heterogeneous lookup: Get never allocates a key; Set allocates only on insert.
  struct NameHash {
    using is_transparent = void;
    size_t
    operator()(std::string_view name) const noexcept {
      return std::hash<std::string_view>{}(name);
    }
  };
  std::unordered_map<std::string, Parameter, NameHash, std::equal_to<>> values_;

  template <typename T>
  void
  SetType(const char *name, T value) {
    const std::string_view key(name);
    if (auto iter = values_.find(key); iter != values_.end())
      iter->second = value;
    else
      values_.emplace(std::string(key), value);
  }

  template <typename T>
  NVNGX_RESULT
  GetType(const char *name, T *out_value) const {

    auto iter = values_.find(std::string_view(name));
    if (iter == values_.end()) {
      return NVNGX_RESULT_FAIL;
    };
    auto res = std::visit(
        overloads{
            [out_value](T value) {
              *out_value = value;
              return NVNGX_RESULT_OK;
            },
            [out_value](auto value) { return castable(value, out_value) ? NVNGX_RESULT_OK : NVNGX_RESULT_FAIL; }
        },
        iter->second
    );
    return res;
  }
};

} // namespace dxmt