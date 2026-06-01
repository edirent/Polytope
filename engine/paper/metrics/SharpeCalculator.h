#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace trading_engine::paper {

enum class MetricStatus : std::uint8_t {
    Ok,
    InsufficientData,
    InvalidInput
};

struct MetricValue {
    double value = 0.0;
    MetricStatus status = MetricStatus::InsufficientData;
};

class SharpeCalculator {
public:
    explicit SharpeCalculator(
        std::size_t min_samples = 2,
        double annualization_factor = 1.0
    )
        : min_samples_(min_samples),
          annualization_factor_(annualization_factor) {}

    [[nodiscard]] MetricValue compute(std::span<const double> returns) const {
        if (returns.size() < min_samples_ || returns.size() < 2) {
            return {};
        }

        double sum = 0.0;
        for (const auto value : returns) {
            sum += value;
        }
        const auto mean = sum / static_cast<double>(returns.size());

        double variance_sum = 0.0;
        for (const auto value : returns) {
            const auto diff = value - mean;
            variance_sum += diff * diff;
        }
        const auto variance =
            variance_sum / static_cast<double>(returns.size() - 1);
        if (variance <= 0.0) {
            return {};
        }

        const auto stddev = std::sqrt(variance);
        if (stddev == 0.0) {
            return {};
        }

        return MetricValue{
            (mean / stddev) * std::sqrt(annualization_factor_),
            MetricStatus::Ok
        };
    }

private:
    std::size_t min_samples_ = 2;
    double annualization_factor_ = 1.0;
};

}  // namespace trading_engine::paper
