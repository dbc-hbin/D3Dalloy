#pragma once

#include "owned_shader_bytecode.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace d3dalloy {

// A synchronous lookup borrows large input spans. Only cache admission freezes
// them into immutable storage. Scalar bytes and canonical ordering stay exactly
// the same as the old contiguous key, including the persistent SHA-1 identity.
class CanonicalStageKey {
  struct Part {
    size_t offset = 0, length = 0;
    const char *external = nullptr;
    SharedShaderBytecode snapshot;
    std::shared_ptr<const std::string> filled;
  };
  std::string scalars_;
  std::vector<Part> parts_;
  size_t size_ = 0;
  mutable std::optional<Sha1Digest> digest_;
  mutable std::optional<std::string> contiguous_;

  std::string_view View(const Part &part) const {
    return {part.external ? part.external : scalars_.data() + part.offset, part.length};
  }
  void Extend(size_t length) {
    if (length > SIZE_MAX - size_) throw std::bad_alloc();
    size_ += length; digest_.reset(); contiguous_.reset();
  }
public:
  CanonicalStageKey() = default;
  CanonicalStageKey(const std::string &value) { append(value.data(), value.size()); }
  CanonicalStageKey(const char *value) { append(value, std::strlen(value)); }

  void append(const char *data, size_t length) {
    if (!length) return;
    const size_t offset = scalars_.size();
    scalars_.append(data, length);
    if (!parts_.empty() && !parts_.back().external &&
        parts_.back().offset + parts_.back().length == offset) parts_.back().length += length;
    else parts_.push_back(Part{offset, length, nullptr, {}, {}});
    Extend(length);
  }
  void reference(const void *data, size_t length) {
    if (!length) return;
    parts_.push_back(Part{0, length, static_cast<const char *>(data), {}, {}});
    Extend(length);
  }
  // Compiler-owned output may be outside the PE address space. Fill one final
  // allocation through the existing copy ABI, then share it without another copy.
  template <typename Fill> bool filled(size_t length, Fill fill) {
    auto data = std::make_shared<std::string>(length, char(0));
    if (!fill(data->data())) return false;
    if (length) {
      parts_.push_back(Part{0, length, data->data(), {}, std::move(data)});
      Extend(length);
    }
    return true;
  }
  const char *lastData() const { return parts_.empty() ? nullptr : View(parts_.back()).data(); }
  size_t size() const { return size_; }
  bool empty() const { return !size_; }
  Sha1Digest digest() const {
    if (!digest_) {
      Sha1HashState state;
      for (const auto &part : parts_) { auto view = View(part); state.update(view.data(), view.size()); }
      digest_ = state.final();
    }
    return *digest_;
  }
  const std::string &string() const {
    if (!contiguous_) {
      std::string result; result.reserve(size_);
      for (const auto &part : parts_) result.append(View(part));
      contiguous_ = std::move(result);
    }
    return *contiguous_;
  }
  CanonicalStageKey owned(ShaderBytecodePool &pool) const {
    CanonicalStageKey result;
    result.scalars_ = scalars_; result.parts_ = parts_; result.size_ = size_;
    result.digest_ = digest();
    for (auto &part : result.parts_) {
      if (!part.external || part.filled || !part.snapshot.empty()) continue;
      part.snapshot = pool.Capture(part.external, part.length);
      part.external = reinterpret_cast<const char *>(part.snapshot.data());
    }
    return result;
  }
  friend bool operator==(const CanonicalStageKey &a, const CanonicalStageKey &b) {
    if (a.size_ != b.size_) return false;
    size_t ai = 0, bi = 0, ao = 0, bo = 0;
    while (ai < a.parts_.size()) {
      const auto av = a.View(a.parts_[ai]), bv = b.View(b.parts_[bi]);
      const size_t length = std::min(av.size() - ao, bv.size() - bo);
      if (av.data() + ao != bv.data() + bo && std::memcmp(av.data() + ao, bv.data() + bo, length)) return false;
      ao += length; bo += length;
      if (ao == av.size()) { ++ai; ao = 0; }
      if (bo == bv.size()) { ++bi; bo = 0; }
    }
    return true;
  }
  struct Hash {
    size_t operator()(const CanonicalStageKey &key) const {
      auto digest = key.digest(); size_t value = 0;
      std::memcpy(&value, digest.data, sizeof(value)); return value;
    }
  };
};

} // namespace d3dalloy
