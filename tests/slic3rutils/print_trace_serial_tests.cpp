// Released under the terms of the AGPLv3 or higher.
#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__) || defined(__linux__)
#include "slic3r/GUI/PrintTrace.hpp"
#include "slic3r/GUI/PrintTraceSerial.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <random>
#include <string>
#include <unistd.h>

namespace {

using Slic3r::GUI::PrintTrace::SerialReader;
using Slic3r::GUI::PrintTrace::Store;

struct TraceDirectory
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prusa-serial-trace-test-" + std::to_string(std::random_device{}()));
    ~TraceDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct PseudoTerminal
{
    ~PseudoTerminal() { close_master(); }

    void close_master()
    {
        if (master >= 0) {
            ::close(master);
            master = -1;
        }
    }

    int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
};

} // namespace

TEST_CASE("USB data updates live trace history and survives a disconnect", "[print_trace]")
{
    using namespace std::chrono_literals;

    PseudoTerminal terminal;
    REQUIRE(terminal.master >= 0);
    REQUIRE(::grantpt(terminal.master) == 0);
    REQUIRE(::unlockpt(terminal.master) == 0);
    const char* slave_name = ::ptsname(terminal.master);
    REQUIRE(slave_name != nullptr);

    TraceDirectory directory;
    Store store(directory.path);
    std::mutex mutex;
    std::condition_variable changed;
    std::string incoming;
    std::error_code disconnected;
    SerialReader reader;
    const auto started = reader.start(slave_name,
        [&](std::string_view bytes) {
            store.append_live(bytes);
            std::lock_guard<std::mutex> lock(mutex);
            incoming.append(bytes);
            changed.notify_all();
        },
        [&](std::error_code error) {
            store.end_live_capture();
            std::lock_guard<std::mutex> lock(mutex);
            disconnected = error;
            changed.notify_all();
        });
    REQUIRE_FALSE(started);
    REQUIRE(reader.connected());

    const std::string first = "start\r\nTRACE,0,sel";
    const std::string next = "ected,1820\r\nTRACE,100,ram_sample,1750\r\n";
    REQUIRE(::write(terminal.master, first.data(), first.size()) == ssize_t(first.size()));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return incoming.size() >= first.size(); }));
        REQUIRE(incoming == first);
    }
    CHECK_FALSE(store.live_run());
    REQUIRE(::write(terminal.master, next.data(), next.size()) == ssize_t(next.size()));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return incoming.size() >= first.size() + next.size(); }));
        REQUIRE(incoming == first + next);
    }
    REQUIRE(store.live_run());
    const auto runs = store.snapshot();
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().printer_status == "printing");
    REQUIRE(runs.front().printer.size() == 2);
    CHECK(runs.front().printer.back().free_bytes == 1750);

    struct pollfd echo { terminal.master, POLLIN, 0 };
    CHECK(::poll(&echo, 1, 0) == 0);

    terminal.close_master();
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return bool(disconnected); }));
    }
    CHECK_FALSE(reader.connected());
    reader.stop();
    CHECK_FALSE(store.live_run());
    const auto saved = Store(directory.path).snapshot();
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().id == runs.front().id);
    CHECK(saved.front().printer_status == "interrupted");
    CHECK(saved.front().capture_has_gaps);
}

TEST_CASE("Stopping printer trace during an idle connection closes the reader without reporting a fault", "[print_trace]")
{
    PseudoTerminal terminal;
    REQUIRE(terminal.master >= 0);
    REQUIRE(::grantpt(terminal.master) == 0);
    REQUIRE(::unlockpt(terminal.master) == 0);
    const char* slave_name = ::ptsname(terminal.master);
    REQUIRE(slave_name != nullptr);

    bool unexpected_disconnect = false;
    SerialReader reader;
    auto ignore_bytes = [](std::string_view) {};
    auto on_disconnect = [&](std::error_code) { unexpected_disconnect = true; };
    REQUIRE_FALSE(reader.start(slave_name, ignore_bytes, on_disconnect));
    CHECK(reader.start(slave_name, ignore_bytes, on_disconnect) == std::errc::device_or_resource_busy);
    reader.stop();
    CHECK_FALSE(reader.connected());
    CHECK_FALSE(unexpected_disconnect);

    REQUIRE_FALSE(reader.start(slave_name, ignore_bytes, on_disconnect));
    CHECK(reader.connected());
    reader.stop();
    CHECK_FALSE(unexpected_disconnect);

    CHECK(reader.start("/dev/null", ignore_bytes, on_disconnect));
    CHECK_FALSE(reader.connected());
}

#endif
