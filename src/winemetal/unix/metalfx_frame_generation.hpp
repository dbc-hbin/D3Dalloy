#pragma once
#include <cstdint>
#include <memory>

namespace d3dalloy::framegeneration {
class ExecutionLease;

class PreparedFrame final {
public:
    struct Impl;

    ~PreparedFrame();
    PreparedFrame(const PreparedFrame&) = delete;
    PreparedFrame& operator=(const PreparedFrame&) = delete;

    bool encode(void* commandBuffer, void* fence,
                std::shared_ptr<const ExecutionLease>& lease, std::uint64_t recording) const noexcept;

private:
    friend std::uint32_t api(std::uint32_t, void*, std::shared_ptr<const PreparedFrame>*) noexcept;
    explicit PreparedFrame(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

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

bool available() noexcept;
std::uint32_t api(std::uint32_t operation, void* arguments, std::shared_ptr<const PreparedFrame>* recorded) noexcept;

} // namespace d3dalloy::framegeneration

