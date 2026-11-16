// Released under the terms of the AGPLv3 or higher.
#ifndef slic3r_GUI_CPAMTracePlot_hpp_
#define slic3r_GUI_CPAMTracePlot_hpp_

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r::GUI::CPAMTracePlot {

struct RamSample {
    std::uint64_t ms;
    int free_bytes;
};

struct RamPoint {
    RamSample sample;
    bool new_segment;
};

inline std::uint64_t startup_end(std::uint64_t last_ms, std::uint64_t startup_ms)
{
    const auto startup = std::min(startup_ms, last_ms);
    const auto padding = std::max<std::uint64_t>(250, startup / 10);
    const auto padded = startup + std::min(last_ms - startup, padding);
    return std::max<std::uint64_t>(1, std::min(last_ms, std::max<std::uint64_t>(2500, padded)));
}

// Emit the first, last and extrema of each pixel bucket. Start a new segment
// after a missed interval so downsampling cannot hide a gap.
inline std::vector<RamPoint> ram_points(const std::vector<RamSample>& samples,
    std::uint64_t end_ms, int width)
{
    constexpr std::uint64_t max_sample_gap_ms = 150;
    std::vector<RamPoint> points;
    if (end_ms == 0 || width <= 0) return points;
    const auto bucket = [end_ms, width](const RamSample& sample) {
        return static_cast<int>((static_cast<long double>(sample.ms) / end_ms) * (width - 1));
    };

    for (std::size_t first = 0; first < samples.size() && samples[first].ms <= end_ms;) {
        const bool new_segment = first == 0 || samples[first].ms - samples[first - 1].ms > max_sample_gap_ms;
        const int pixel = bucket(samples[first]);
        std::size_t last = first, minimum = first, maximum = first;
        while (last + 1 < samples.size() && samples[last + 1].ms <= end_ms &&
               samples[last + 1].ms - samples[last].ms <= max_sample_gap_ms &&
               bucket(samples[last + 1]) == pixel) {
            ++last;
            if (samples[last].free_bytes < samples[minimum].free_bytes) minimum = last;
            if (samples[last].free_bytes > samples[maximum].free_bytes) maximum = last;
        }
        auto positions = std::array{first, minimum, maximum, last};
        std::sort(positions.begin(), positions.end());
        for (auto it = positions.begin(), end = std::unique(positions.begin(), positions.end()); it != end; ++it)
            points.push_back({samples[*it], new_segment && it == positions.begin()});
        first = last + 1;
    }
    return points;
}

} // namespace Slic3r::GUI::CPAMTracePlot

#endif
