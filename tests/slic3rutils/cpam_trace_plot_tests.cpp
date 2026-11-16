#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/CPAMTracePlot.hpp"

using namespace Slic3r::GUI::CPAMTracePlot;

TEST_CASE("SD trace startup view includes observed startup and stays inside the run", "[print_trace]")
{
    CHECK(startup_end(300000, 3000) == 3300);
    CHECK(startup_end(300000, 440) == 2500);
    CHECK(startup_end(290, 400) == 290);
    CHECK(startup_end(0, 0) == 1);
}

TEST_CASE("SD trace chart retains RAM extremes and shows missing samples", "[print_trace]")
{
    const std::vector<RamSample> samples{
        {0, 1880}, {100, 1570}, {200, 1920}, {300, 1800},
        {600, 1600}, {700, 1770}, {800, 1630}, {900, 1900}
    };
    const auto points = ram_points(samples, 800, 1);
    REQUIRE(points.size() == 7);
    CHECK(points[0].new_segment);
    CHECK(points[0].sample.ms == 0);
    CHECK(points[1].sample.free_bytes == 1570);
    CHECK(points[2].sample.free_bytes == 1920);
    CHECK(points[4].new_segment);
    CHECK(points[4].sample.ms == 600);
    CHECK(points[6].sample.ms == 800);
    CHECK(ram_points({}, 1, 1).empty());
}
