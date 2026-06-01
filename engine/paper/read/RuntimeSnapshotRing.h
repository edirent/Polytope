#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace trading_engine::paper {

template <typename Snapshot>
class RuntimeSnapshotRing {
public:
    explicit RuntimeSnapshotRing(std::size_t capacity)
        : capacity_(std::max<std::size_t>(capacity, 1)) {
        slots_.reserve(capacity_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            slots_.push_back(std::make_unique<Slot>());
        }
    }

    [[nodiscard]] std::uint64_t publish(Snapshot snapshot) {
        const auto seq = next_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        snapshot.seq_no = seq;

        auto& slot = *slots_[index_for(seq)];
        std::unique_lock lock{slot.mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        slot.snapshot = std::move(snapshot);
        slot.seq = seq;
        latest_published_seq_.store(seq, std::memory_order_release);
        return seq;
    }

    [[nodiscard]] std::optional<Snapshot> latest() const {
        const auto latest_seq =
            latest_published_seq_.load(std::memory_order_acquire);
        if (latest_seq == 0) {
            return std::nullopt;
        }

        const auto min_seq = oldest_available_seq(latest_seq);
        for (auto seq = latest_seq; seq >= min_seq; --seq) {
            if (auto snapshot = read_exact(seq)) {
                return snapshot;
            }
            if (seq == 0) {
                break;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<Snapshot> read_since(
        std::uint64_t since_seq,
        std::size_t max_count = std::numeric_limits<std::size_t>::max()
    ) const {
        std::vector<Snapshot> out;
        if (max_count == 0) {
            return out;
        }

        const auto latest_seq =
            latest_published_seq_.load(std::memory_order_acquire);
        if (latest_seq == 0 || since_seq >= latest_seq) {
            return out;
        }

        const auto min_seq = oldest_available_seq(latest_seq);
        const auto start_seq = std::max(since_seq + 1, min_seq);
        const auto expected_count = static_cast<std::size_t>(
            latest_seq >= start_seq ? latest_seq - start_seq + 1 : 0
        );
        out.reserve(std::min(expected_count, max_count));

        for (auto seq = start_seq; seq <= latest_seq && out.size() < max_count;
             ++seq) {
            if (auto snapshot = read_exact(seq)) {
                out.push_back(std::move(*snapshot));
            }
        }
        return out;
    }

    [[nodiscard]] std::uint64_t latest_seq() const noexcept {
        return latest_published_seq_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
        return dropped_frames_.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        mutable std::mutex mutex;
        std::uint64_t seq = 0;
        Snapshot snapshot;
    };

    [[nodiscard]] std::size_t index_for(std::uint64_t seq) const noexcept {
        return static_cast<std::size_t>((seq - 1) % capacity_);
    }

    [[nodiscard]] std::uint64_t oldest_available_seq(
        std::uint64_t latest_seq
    ) const noexcept {
        if (latest_seq <= capacity_) {
            return 1;
        }
        return latest_seq - static_cast<std::uint64_t>(capacity_) + 1;
    }

    [[nodiscard]] std::optional<Snapshot> read_exact(std::uint64_t seq) const {
        auto& slot = *slots_[index_for(seq)];
        std::unique_lock lock{slot.mutex, std::try_to_lock};
        if (!lock.owns_lock() || slot.seq != seq) {
            return std::nullopt;
        }
        return slot.snapshot;
    }

    std::size_t capacity_ = 1;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<std::uint64_t> next_seq_{0};
    std::atomic<std::uint64_t> latest_published_seq_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
};

}  // namespace trading_engine::paper
