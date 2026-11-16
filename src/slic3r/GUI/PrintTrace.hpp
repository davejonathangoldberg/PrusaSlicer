// Released under the terms of the AGPLv3 or higher.
#ifndef slic3r_GUI_PrintTrace_hpp_
#define slic3r_GUI_PrintTrace_hpp_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::GUI::PrintTrace {

using SteadyClock = std::chrono::steady_clock;
struct SaveTime {
    SteadyClock::time_point steady;
    int64_t unix_ms;
    static SaveTime now();
};

struct Event {
    std::string name;
    uint64_t elapsed_ms = 0;
    std::optional<int> free_bytes;
};

struct Run {
    uint64_t id = 0;
    std::string name;
    int64_t started_at_unix_ms = 0;
    std::string export_status;
    std::string printer_status;
    std::vector<Event> slicer;
    std::vector<Event> printer;
    uint64_t dropped_samples = 0;
    uint64_t dropped_events = 0;
    bool capture_has_gaps = false;
    bool removable = false;
};

struct ImportResult {
    size_t imported = 0;
    std::string error;
};

class Store
{
public:
    explicit Store(std::filesystem::path directory);
    std::vector<Run> snapshot() const;
    uint64_t revision() const;
    std::string storage_error() const;

    uint64_t begin_export(std::string name, bool removable, SaveTime confirmed);
    void record_export(uint64_t id, std::string_view event, SteadyClock::time_point time);
    void end_export(uint64_t id, std::string_view status, std::string_view event,
                    SteadyClock::time_point time, std::string_view final_name = {});
    ImportResult import_printer_log(std::istream& input, std::string name,
                                   std::optional<uint64_t> export_id = {});

private:
    void load();
    bool persist(const Run& run, bool attachment = false);
    uint64_t allocate_id();

    const std::filesystem::path m_directory;
    mutable std::mutex m_mutex;
    std::map<uint64_t, Run> m_runs;
    std::map<uint64_t, SteadyClock::time_point> m_export_starts;
    uint64_t m_revision = 0;
    uint64_t m_next_id = 0;
    std::string m_storage_error;
};

Store& store();

// Recorder failures must never change the outcome of a G-code export.
uint64_t begin_export(std::string_view path, bool removable, SaveTime confirmed) noexcept;
void record_export(uint64_t id, std::string_view event) noexcept;
void end_export(uint64_t id, std::string_view status, std::string_view event,
                std::string_view final_path = {}) noexcept;

} // namespace Slic3r::GUI::PrintTrace

#endif
