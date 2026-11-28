#ifndef HARKEN_API_CALLBACK_BYTE_SOURCE_H
#define HARKEN_API_CALLBACK_BYTE_SOURCE_H

#include <harken/Harken.h>

#include "harken/media/StreamByteSource.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace harken::detail {

class CallbackByteSource final : public StreamByteSource {
public:
    VnByteSourceDescV1 desc;
    std::atomic<bool> armed{false};

    explicit CallbackByteSource(const VnByteSourceDescV1& input) : desc(input) {}
    ~CallbackByteSource() override {
        if (armed.load(std::memory_order_acquire)) {
            try { desc.release(desc.userData); } catch (...) {}
        }
    }

    std::uint64_t ByteCount() const noexcept override { return desc.byteLength; }

    static std::uint32_t IsCancelled(const void* context) {
        return static_cast<const StreamReadCancellation*>(context)->IsCancelled() ? 1u : 0u;
    }

    StreamReadResult ReadAt(std::uint64_t offset, void* destination, std::size_t count,
        StreamReadCancellation cancellation) const override {
        if (cancellation.IsCancelled()) return {StreamReadStatus::Cancelled, 0};
        VnReadCancellationViewV1 view{sizeof(view), 0, &cancellation, &IsCancelled};
        std::uint32_t read = 0;
        try {
            const auto requested = static_cast<std::uint32_t>(std::min<std::size_t>(count, UINT32_MAX));
            const auto result = desc.readAt(desc.userData, offset, destination, requested, &read, &view);
            if (cancellation.IsCancelled() || result == VN_BYTE_READ_CANCELLED)
                return {StreamReadStatus::Cancelled, 0};
            if (result != VN_BYTE_READ_OK || read > requested) return {};
            return {StreamReadStatus::Ok, read};
        } catch (...) { return {}; }
    }
};

} // namespace harken::detail

#endif // HARKEN_API_CALLBACK_BYTE_SOURCE_H
