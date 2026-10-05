#pragma once

#include "metalfx-contract.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace yaagl::pso::metalfx {

enum class CommandMode : std::uint8_t {
    Legacy,
    Metal4,
};

struct CreateContext {
    void* device = nullptr;
    void* compiler = nullptr;
    CommandMode mode = CommandMode::Metal4;
};

// Native Metal views resolved by the transport layer from the exact D3D12
// resources/views supplied to one temporal-upscaling dispatch. The backend retains every
// non-null object captured by a PreparedFrame until that frame is destroyed.
struct TextureSet {
    void* color = nullptr;
    void* depth = nullptr;
    void* motion = nullptr;
    void* output = nullptr;
    void* exposure = nullptr;
    void* reactive = nullptr;
    void* composition = nullptr;
};

enum class ColorTransfer : std::uint8_t {
    Linear,
    SRGB,
    PQ,
};

struct FrameOperations {
    ColorTransfer colorTransfer = ColorTransfer::Linear;
    bool combineCompositionMask = false;
    bool sharpening = false;
    float sharpness = 0.0f;
    // FSR-only opt-in. When the caller output exceeds the device's public
    // temporal scale limit, run MetalFX at the largest exact input multiple and
    // composite it unscaled into the centered, unchanged caller output.
    bool capOutputToTemporalMaxScale = false;
    bool drawDebugView = false;
};

enum class ErrorCode : std::uint8_t {
    None,
    InvalidContext,
    UnsupportedFeature,
    InvalidFrame,
    IncompatibleTexture,
    ResourceCreationFailed,
    ScalerCreationFailed,
    EncodeFailed,
};

struct Error {
    ErrorCode code = ErrorCode::None;
    std::string message;

    explicit operator bool() const noexcept { return code != ErrorCode::None; }
};

class PreparedFrame;
class ExecutionLease;

class Feature final {
public:
    struct Impl;

    static std::shared_ptr<Feature> create(const CreateContext& context,
                                           const CreateInfo& info,
                                           Error* error = nullptr) noexcept;

    ~Feature();

    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;

    // Performs all validation and allocations that can fail before the native
    // recorded command is appended. A successful result is immutable and safe
    // for the transport to retain through GPU completion.
    std::shared_ptr<const PreparedFrame> prepare(
        const FrameInfo& info, const TextureSet& textures, Error* error = nullptr,
        const FrameOperations& operations = {}) noexcept;

    CommandMode mode() const noexcept;

    // Called when the owner drops the feature. Recorded frames may still prepare
    // their encode and replay, but idle scratch is drained now and later
    // GPU-completion returns are released instead of pooled.
    void markDormant() noexcept;

private:
    friend class PreparedFrame;
    explicit Feature(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

class PreparedFrame final {
public:
    struct Impl;

    ~PreparedFrame();

    PreparedFrame(const PreparedFrame&) = delete;
    PreparedFrame& operator=(const PreparedFrame&) = delete;

    // commandBuffer is the caller's already-begun MTL4CommandBuffer for
    // Metal4, or MTLCommandBuffer for Legacy. fence is the exact dependency
    // fence supplied by the transport. The caller must retain this object until
    // GPU completion; command-buffer encode return alone is not a lifetime
    // boundary for Metal4. lease is published before the first GPU command is
    // recorded. The transport must retain any non-null lease even when encode
    // returns false, because a later step can fail after earlier commands have
    // captured transient resources.
    bool encode(void* commandBuffer, void* fence,
                std::shared_ptr<const ExecutionLease>& lease,
                Error* error = nullptr, bool forceReset = false,
                void* splitContext = nullptr, void* (*splitCommandBuffer)(void*) = nullptr) const noexcept;
    std::uint64_t generationId() const noexcept;

    CommandMode mode() const noexcept;

private:
    friend class Feature;
    explicit PreparedFrame(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

// One GPU execution's transient resources, including its scaler generation.
// The transport retains the returned lease until the corresponding native
// allocator is reset after GPU completion.
// Keeping this separate from PreparedFrame prevents resource aliasing when one
// recorded command is replayed into more than one in-flight command buffer.
class ExecutionLease final {
public:
    struct Impl;

    ~ExecutionLease();

    ExecutionLease(const ExecutionLease&) = delete;
    ExecutionLease& operator=(const ExecutionLease&) = delete;

private:
    friend class PreparedFrame;
    explicit ExecutionLease(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace yaagl::pso::metalfx
