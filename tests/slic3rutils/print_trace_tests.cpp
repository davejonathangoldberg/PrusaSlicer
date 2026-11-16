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

TEST_CASE("Live USB records update a linked export and survive restart", "[PrintTrace]")
{
    TraceDirectory dir;
    Store store(dir.path);
    const auto now = SaveTime::now();
    const auto export_id = store.begin_export("job.gcode", true, now);
    store.end_export(export_id, "gcode_saved", "gcode_on_sd", now.steady);
    Store stale_store(dir.path);
    REQUIRE(store.track_next_print(export_id));
    CHECK(store.tracked_export() == export_id);

    const auto revision = store.revision();
    store.append_live("start\r\nTRACE,0,sele");
    CHECK_FALSE(store.live_run());
    CHECK(store.revision() == revision);
    store.append_live("cted,1840\r\nTRACE,100,ram_sample,17");
    CHECK_FALSE(store.tracked_export());
    REQUIRE(store.live_run() == export_id);
    auto runs = store.snapshot();
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().printer_status == "printing");
    REQUIRE(runs.front().printer.size() == 1);

    store.append_live("50\nTRACE,200,finished,1820\n");
    store.append_live("TRACE_LOSS,100,1,2\n");
    std::istringstream competing("TRACE,0,selected,1840\nTRACE,50,finished,1800\n");
    CHECK(stale_store.import_printer_log(competing, "competing", export_id).imported == 0);
    CHECK_FALSE(store.track_next_print(export_id));
    store.end_live_capture();
    store.end_live_capture();
    CHECK_FALSE(store.live_run());

    runs = Store(dir.path).snapshot();
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().id == export_id);
    CHECK(runs.front().export_status == "gcode_saved");
    CHECK(runs.front().printer_status == "completed");
    REQUIRE(runs.front().printer.size() == 3);
    CHECK(runs.front().printer[1].free_bytes == 1750);
    CHECK(runs.front().dropped_samples == 1);
    CHECK(runs.front().dropped_events == 2);
    CHECK(runs.front().capture_has_gaps);
}

TEST_CASE("Live capture separates incomplete attempts across startup and disconnect", "[PrintTrace]")
{
    TraceDirectory dir;
    Store store(dir.path);
    const auto now = SaveTime::now();
    const auto export_id = store.begin_export("later.gcode", true, now);
    store.end_export(export_id, "gcode_saved", "gcode_on_sd", now.steady);
    REQUIRE(store.track_next_print(export_id));
    store.append_live("TRACE,100,ram_sample,1800\n");
    auto partial = store.live_run();
    REQUIRE(partial);
    CHECK(partial != export_id);
    CHECK(store.tracked_export() == export_id);
    store.append_live("start\n");
    CHECK_FALSE(store.live_run());
    CHECK_FALSE(store.tracked_export());

    store.append_live("TRACE,0,selected,1700\nTRACE,100,ram_sample,1690\n");
    auto second = store.live_run();
    REQUIRE(second);
    CHECK(second != partial);
    store.append_live("TRACE," + std::string(8192, 'x') + "\nTRACE,200,ram_sample,1680\n");
    store.append_live("TRACE,300,finished,1");
    store.end_live_capture();

    const auto runs = Store(dir.path).snapshot();
    REQUIRE(runs.size() == 3);
    auto previous = std::find_if(runs.begin(), runs.end(), [&](const Run& run) { return run.id == partial; });
    auto interrupted = std::find_if(runs.begin(), runs.end(), [&](const Run& run) { return run.id == second; });
    REQUIRE(previous != runs.end());
    REQUIRE(interrupted != runs.end());
    CHECK(previous->printer_status == "interrupted");
    CHECK(previous->capture_has_gaps);
    CHECK(interrupted->printer_status == "interrupted");
    CHECK(interrupted->capture_has_gaps);
    REQUIRE(interrupted->printer.size() == 3);
    CHECK(interrupted->printer.back().elapsed_ms == 200);
}

TEST_CASE("Live capture recovers checked-in samples after restart", "[PrintTrace]")
{
    TraceDirectory dir;
    Store store(dir.path);
    store.append_live("TRACE,0,selected,1700\nTRACE,100,ram_sample,1650\n");

    const auto runs = Store(dir.path).snapshot();
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().printer_status == "interrupted");
    CHECK(runs.front().capture_has_gaps);
    REQUIRE(runs.front().printer.size() == 2);
    CHECK(runs.front().printer.back().free_bytes == 1650);
}
