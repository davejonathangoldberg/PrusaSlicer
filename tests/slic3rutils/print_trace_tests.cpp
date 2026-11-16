#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/PrintTrace.hpp"

#include <algorithm>
#include <filesystem>
#include <random>
#include <sstream>

namespace {
using namespace Slic3r::GUI::PrintTrace;

struct TraceDirectory
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("slicer-print-trace-test-" + std::to_string(std::random_device{}()));
    ~TraceDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}

TEST_CASE("Trace history keeps checked SD exports and an associated print after restart", "[PrintTrace]")
{
    TraceDirectory dir;
    Store store(dir.path);
    const auto now = SaveTime::now();
    const auto id = store.begin_export("draft.gcode", true, now);
    store.record_export(id, "gcode_ready", now.steady + std::chrono::milliseconds(200));
    store.record_export(id, "sd_write_started", now.steady + std::chrono::milliseconds(250));
    store.end_export(id, "gcode_saved", "gcode_on_sd", now.steady + std::chrono::milliseconds(312), "final.gcode");

    Store stale_store(dir.path);
    std::istringstream capture("start\r\nTRACE,0,selected,1840\r\nTRACE,100,ram_sample,1830\r\n"
                               "TRACE,115,unknown_event,1800\r\nTRACE,200,finished,1812\r\nTRACE_LOSS,100,2,1\r\n");
    REQUIRE(store.import_printer_log(capture, "capture.log", id).imported == 1);
    std::istringstream competing_capture("TRACE,0,selected,1840\nTRACE,50,finished,1800\n");
    CHECK(stale_store.import_printer_log(competing_capture, "competing", id).imported == 0);

    Store reloaded(dir.path);
    const auto runs = reloaded.snapshot();
    REQUIRE(runs.size() == 1);
    const auto& run = runs.front();
    CHECK(run.name == "final.gcode");
    CHECK(run.removable);
    CHECK(run.export_status == "gcode_saved");
    REQUIRE(run.slicer.size() == 4);
    CHECK(run.slicer.back().elapsed_ms == 312);
    CHECK(run.printer_status == "completed");
    REQUIRE(run.printer.size() == 4);
    CHECK(run.printer[1].free_bytes == 1830);
    CHECK(run.capture_has_gaps);
    CHECK(run.dropped_samples == 2);
    CHECK(run.dropped_events == 1);

    std::istringstream another("TRACE,0,selected,1840\nTRACE,10,cancelled,1820\n");
    CHECK(reloaded.import_printer_log(another, "second", id).imported == 0);
    std::istringstream standalone("TRACE,0,selected,1840\nTRACE,10,cancelled,1820\n");
    REQUIRE(reloaded.import_printer_log(standalone, "second").imported == 1);
    CHECK(reloaded.snapshot().size() == 2);
}

TEST_CASE("Trace import keeps distinct print attempts and reports missing data", "[PrintTrace]")
{
    TraceDirectory dir;
    Store store(dir.path);
    const auto now = SaveTime::now();
    const auto id = store.begin_export("local.gcode", false, now);
    store.end_export(id, "failed", "export_failed", now.steady);

    const char* log = "TRACE,0,selected,1840\nTRACE,not-a-time,ram_sample,1500\n"
                      "TRACE,20,sd_check_start,1820\nTRACE,0,selected,1850\n"
                      "TRACE,12,start_failed,1825\nTRACE,100,ram_sample,1";
    std::istringstream attempts_for_export(log);
    CHECK(store.import_printer_log(attempts_for_export, "two attempts", id).imported == 0);
    std::istringstream attempts(log);
    REQUIRE(store.import_printer_log(attempts, "two attempts").imported == 2);

    const auto runs = Store(dir.path).snapshot();
    REQUIRE(runs.size() == 3);
    CHECK(std::count_if(runs.begin(), runs.end(), [](const Run& run) {
        return run.printer_status == "incomplete" && run.capture_has_gaps;
    }) == 1);
    CHECK(std::count_if(runs.begin(), runs.end(), [](const Run& run) {
        return run.printer_status == "start_failed" && run.capture_has_gaps && run.printer.size() == 2;
    }) == 1);
    CHECK(std::count_if(runs.begin(), runs.end(), [](const Run& run) {
        return run.export_status == "failed" && !run.removable;
    }) == 1);
}
