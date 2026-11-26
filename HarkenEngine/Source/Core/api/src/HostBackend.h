#ifndef HARKEN_API_HOST_BACKEND_H
#define HARKEN_API_HOST_BACKEND_H

#include <harken/Harken.h>

#include "RenderSlicer.h"
#include "harken/core/EngineConfig.h"

#include <cstdint>
#include <vector>

namespace harken {

class MixerLoop;

class HostBackend {
public:
    HostBackend(MixerLoop& mixer, const EngineConfig& config);
    ~HostBackend();

    HostBackend(const HostBackend&) = delete;
    HostBackend& operator=(const HostBackend&) = delete;

    void Admit() noexcept;
    void Revoke() noexcept;

    static VnResult Render(float* interleavedOut, std::uint32_t frameCount,
                           std::uint32_t channelCount) noexcept;

private:
    VnResult RenderAdmitted(float* interleavedOut, std::uint32_t frameCount,
                            std::uint32_t channelCount) noexcept;

    MixerLoop& m_mixer;
    std::uint32_t m_blockFrames;
    std::uint32_t m_channels;
    std::vector<float> m_staging;
    BlockStagingCursor m_cursor;
};

} // namespace harken

#endif // HARKEN_API_HOST_BACKEND_H
