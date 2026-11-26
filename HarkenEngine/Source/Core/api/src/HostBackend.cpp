#include "HostBackend.h"

#include "harken/core/AllocGuard.h"
#include "harken/core/FloatMode.h"
#include "harken/render/MixerLoop.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>

namespace harken {

namespace {

std::atomic<bool> g_hostRenderActive{false};
std::atomic<HostBackend*> g_hostRenderAdmitted{nullptr};

} // namespace

HostBackend::HostBackend(MixerLoop& mixer, const EngineConfig& config)
    : m_mixer(mixer),
      m_blockFrames(config.blockFrames),
      m_channels(config.deviceChannelCount),
      m_staging(static_cast<std::size_t>(config.blockFrames) *
                    config.deviceChannelCount,
                0.0f) {}

HostBackend::~HostBackend() {
    Revoke();
}

void HostBackend::Admit() noexcept {
    g_hostRenderAdmitted.store(this, std::memory_order_seq_cst);
}

void HostBackend::Revoke() noexcept {
    HostBackend* expected = this;
    if (!g_hostRenderAdmitted.compare_exchange_strong(
            expected, nullptr, std::memory_order_seq_cst)) {
        return;
    }
    while (g_hostRenderActive.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
    }
}

VnResult HostBackend::Render(float* interleavedOut, std::uint32_t frameCount,
                             std::uint32_t channelCount) noexcept {
    if (interleavedOut == nullptr || channelCount == 0) {
        return VN_INVALID_ARG;
    }
    const std::size_t bytes = static_cast<std::size_t>(frameCount) *
                              channelCount * sizeof(float);
    if (g_hostRenderActive.exchange(true, std::memory_order_seq_cst)) {
        std::memset(interleavedOut, 0, bytes);
        return VN_IN_USE;
    }
    VnResult result = VN_NOT_INITIALIZED;
    HostBackend* backend =
        g_hostRenderAdmitted.load(std::memory_order_seq_cst);
    if (backend != nullptr) {
        result = backend->RenderAdmitted(interleavedOut, frameCount,
                                         channelCount);
    }
    if (result != VN_OK) {
        std::memset(interleavedOut, 0, bytes);
    }
    g_hostRenderActive.store(false, std::memory_order_seq_cst);
    return result;
}

VnResult HostBackend::RenderAdmitted(float* interleavedOut,
                                     std::uint32_t frameCount,
                                     std::uint32_t channelCount) noexcept {
    if (channelCount != m_channels) {
        return VN_INVALID_ARG;
    }
    floatmode::ScopedFlushDenormals floatModeScope;
    memguard::AudioThreadScope audioScope;
    float* staging = m_staging.data();
    const std::uint32_t channels = m_channels;
    PumpStagedBlocks(
        m_cursor, frameCount, m_blockFrames,
        [&] { m_mixer.RenderBlock(staging, m_blockFrames, channels); },
        [&](std::uint32_t dstFrames, std::uint32_t srcFrames,
            std::uint32_t frames) {
            std::memcpy(
                interleavedOut + static_cast<std::size_t>(dstFrames) * channels,
                staging + static_cast<std::size_t>(srcFrames) * channels,
                static_cast<std::size_t>(frames) * channels * sizeof(float));
        });
    return VN_OK;
}

} // namespace harken
