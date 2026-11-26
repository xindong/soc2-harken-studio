#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlocks = 16;
constexpr std::uint32_t kTotalFrames = kBlock * kBlocks;

VnConfig HostConfig() {
    VnConfig config = harkentest::MakeNullConfigLayout(
        0, kBlock, kRate, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO);
    config.deviceMode = VN_DEVICE_HOST;
    return config;
}

void PlaySine() {
    const std::vector<float> sine =
        harkentest::MakeSine(kTotalFrames * 2, 440.0, kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(sine, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
}

std::vector<float> RenderBlockReference() {
    std::vector<float> reference;
    EngineSession session(harkentest::MakeNullConfigLayout(
        0, kBlock, kRate, VN_CHANNEL_LAYOUT_STEREO, VN_CHANNEL_LAYOUT_STEREO));
    EXPECT_EQ(VN_OK, session.InitResult());
    if (session.InitResult() != VN_OK) {
        return reference;
    }
    PlaySine();
    auto& backend = session.Engine()->nullBackend;
    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        backend.Pump(1);
        const float* samples = backend.LastBlock();
        reference.insert(reference.end(), samples,
                         samples + kBlock * kChannels);
    }
    return reference;
}

bool AllSilent(const std::vector<float>& buffer) {
    for (const float sample : buffer) {
        if (sample != 0.0f) {
            return false;
        }
    }
    return true;
}

TEST(HostRender, SegmentedPullsDeliverTheBlockRenderedStream) {
    const std::vector<float> reference = RenderBlockReference();
    ASSERT_EQ(static_cast<std::size_t>(kTotalFrames) * kChannels,
              reference.size());
    ASSERT_FALSE(AllSilent(reference));

    const std::vector<std::vector<std::uint32_t>> patterns = {
        {kBlock},
        {1024},
        {1},
        {100, 700, 3, 512, 255, 257},
        {kTotalFrames},
    };
    for (const auto& pattern : patterns) {
        SCOPED_TRACE(testing::Message() << "first segment " << pattern[0]
                                        << " of " << pattern.size());
        EngineSession session(HostConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(PlaySine());
        std::vector<float> pulled(reference.size(), 9.0f);
        std::uint32_t done = 0;
        std::size_t step = 0;
        while (done < kTotalFrames) {
            std::uint32_t frames = pattern[step++ % pattern.size()];
            if (frames > kTotalFrames - done) {
                frames = kTotalFrames - done;
            }
            ASSERT_EQ(VN_OK,
                      Vn_RenderHostOutput(
                          pulled.data() +
                              static_cast<std::size_t>(done) * kChannels,
                          frames, kChannels));
            done += frames;
        }
        EXPECT_EQ(reference, pulled);
        EXPECT_EQ(0u, harken::memguard::HitCount());
    }
}

TEST(HostRender, InitRequiresExplicitFormatAndNoRenderWorkers) {
    VnConfig noRate = HostConfig();
    noRate.sampleRate = 0;
    VnConfig noLayout = HostConfig();
    noLayout.deviceChannelLayout = VN_CHANNEL_LAYOUT_DEFAULT;
    VnConfig workers = HostConfig();
    workers.renderWorkerCount = 1;
    for (const VnConfig& config : {noRate, noLayout, workers}) {
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
        EXPECT_EQ(VN_NOT_INITIALIZED, Vn_Term());
    }
}

TEST(HostRender, QueryReportsTheSessionFormat) {
    VnHostOutputFormatV1 format{};
    format.structSize = sizeof(format);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_QueryHostOutputFormat(&format));
    {
        EngineSession session(harkentest::MakeNullConfig(0, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_QueryHostOutputFormat(&format));
    }
    EngineSession session(HostConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryHostOutputFormat(nullptr));
    VnHostOutputFormatV1 small{};
    small.structSize = sizeof(small) - 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_QueryHostOutputFormat(&small));
    ASSERT_EQ(VN_OK, Vn_QueryHostOutputFormat(&format));
    EXPECT_EQ(kRate, format.sampleRate);
    EXPECT_EQ(kChannels, format.channelCount);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_CHANNEL_LAYOUT_STEREO),
              format.channelLayout);
    EXPECT_EQ(kBlock, format.blockFrames);
}

TEST(HostRender, RefusedCallsWriteSilence) {
    constexpr std::uint32_t kFrames = 300;
    std::vector<float> buffer(kFrames * kChannels, 9.0f);
    auto refill = [&] { buffer.assign(buffer.size(), 9.0f); };

    EXPECT_EQ(VN_INVALID_ARG, Vn_RenderHostOutput(nullptr, kFrames, kChannels));
    EXPECT_EQ(VN_INVALID_ARG, Vn_RenderHostOutput(buffer.data(), kFrames, 0));

    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_RenderHostOutput(buffer.data(), kFrames, kChannels));
    EXPECT_TRUE(AllSilent(buffer));
    {
        EngineSession session(harkentest::MakeNullConfig(0, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        refill();
        EXPECT_EQ(VN_NOT_INITIALIZED,
                  Vn_RenderHostOutput(buffer.data(), kFrames, kChannels));
        EXPECT_TRUE(AllSilent(buffer));
    }
    {
        EngineSession session(HostConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(PlaySine());
        std::vector<float> mono(kFrames, 9.0f);
        EXPECT_EQ(VN_INVALID_ARG,
                  Vn_RenderHostOutput(mono.data(), kFrames, 1));
        EXPECT_TRUE(AllSilent(mono));
        EXPECT_EQ(0u, session.Engine()->mixer.BlocksRendered());

        ASSERT_EQ(VN_OK, Vn_Suspend());
        refill();
        EXPECT_EQ(VN_NOT_INITIALIZED,
                  Vn_RenderHostOutput(buffer.data(), kFrames, kChannels));
        EXPECT_TRUE(AllSilent(buffer));
        EXPECT_EQ(0u, session.Engine()->mixer.BlocksRendered());

        ASSERT_EQ(VN_OK, Vn_Resume());
        refill();
        EXPECT_EQ(VN_OK,
                  Vn_RenderHostOutput(buffer.data(), kFrames, kChannels));
        EXPECT_FALSE(AllSilent(buffer));
        EXPECT_EQ(2u, session.Engine()->mixer.BlocksRendered());
    }
    refill();
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_RenderHostOutput(buffer.data(), kFrames, kChannels));
    EXPECT_TRUE(AllSilent(buffer));
}

TEST(HostRender, LifecycleChangesWaitForTheRenderThread) {
    std::atomic<bool> stop{false};
    std::atomic<std::uint32_t> unexpected{0};
    std::atomic<std::uint64_t> rendered{0};
    std::thread renderThread([&] {
        std::vector<float> buffer(1024 * kChannels);
        while (!stop.load()) {
            const VnResult result =
                Vn_RenderHostOutput(buffer.data(), 1024, kChannels);
            if (result == VN_OK) {
                rendered.fetch_add(1);
            } else if (result != VN_NOT_INITIALIZED) {
                unexpected.fetch_add(1);
            }
        }
    });
    const VnConfig config = HostConfig();
    for (int session = 0; session < 3; ++session) {
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        const std::uint64_t before = rendered.load();
        while (rendered.load() < before + 8) {
            std::this_thread::yield();
        }
        for (int cycle = 0; cycle < 50; ++cycle) {
            ASSERT_EQ(VN_OK, Vn_Suspend());
            ASSERT_EQ(VN_OK, Vn_Resume());
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        ASSERT_EQ(VN_OK, Vn_Term());
    }
    stop.store(true);
    renderThread.join();
    EXPECT_EQ(0u, unexpected.load());
}

} // namespace
