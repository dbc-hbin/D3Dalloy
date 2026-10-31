// SPDX-License-Identifier: LGPL-2.1-or-later
// AMD Ray Regeneration provider: MetalFX neural radiance denoising plus AMD contract corrections.
#pragma once
#include "metalfx-backend.hpp"
#include "../winemetal_metal4_rr.h"
#include <array>
#include <memory>

namespace dxmt::rr {
using yaagl::pso::metalfx::Error;
using yaagl::pso::metalfx::ErrorCode;
using TextureSet = std::array<void*, WMTRR_RESOURCE_COUNT>;
bool validateSettings(const WMTRRSettings& settings) noexcept;

class PreparedFrame;
class ExecutionLease;

class Feature final {
public:
    struct Impl;
    static std::shared_ptr<Feature> create(void* device, void* compiler,
        const WMTRRCreateInfo& info, Error* error = nullptr) noexcept;
    ~Feature();
    std::shared_ptr<const PreparedFrame> prepare(const WMTRRFrame& frame,
        const TextureSet& textures, const WMTRRSettings& settings, Error* error = nullptr) noexcept;
    std::uint64_t memoryUsage() const noexcept;
    void markDormant() noexcept;
private:
    explicit Feature(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

class PreparedFrame final {
public:
    struct Impl;
    ~PreparedFrame();
    bool encode(void* commandBuffer, void* fence,
        std::shared_ptr<const ExecutionLease>& lease, void* (*split)(void*), void* splitContext,
        Error* error = nullptr) const noexcept;
private:
    friend class Feature;
    explicit PreparedFrame(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

class ExecutionLease final {
public:
    struct Impl;
    ~ExecutionLease();
private:
    friend class PreparedFrame;
    explicit ExecutionLease(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};
} // namespace dxmt::rr
