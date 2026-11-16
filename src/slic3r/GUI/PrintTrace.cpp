// Released under the terms of the AGPLv3 or higher.
#include "PrintTrace.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

namespace Slic3r::GUI::PrintTrace {
namespace {

using Milliseconds = std::chrono::milliseconds;

uint64_t elapsed_ms(SteadyClock::time_point start, SteadyClock::time_point now)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<Milliseconds>(std::max(now, start) - start).count());
}

template<class Number> bool parse_number(std::string_view field, Number& value)
{
    if (field.empty()) return false;
    const auto end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_fields(std::string_view line, std::array<std::string_view, 3>& fields)
{
    for (size_t i = 0; i < fields.size() - 1; ++i) {
        const auto comma = line.find(',');
        if (comma == std::string_view::npos) return false;
        fields[i] = line.substr(0, comma);
        line.remove_prefix(comma + 1);
    }
    fields.back() = line;
    return line.find(',') == std::string_view::npos;
}

bool valid_event_name(std::string_view name)
{
    return !name.empty() && name.size() <= 64 &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
}

std::string safe_name(std::string name)
{
    std::replace_if(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }, ' ');
    return name;
}

std::vector<Run> parse_capture(std::istream& input)
{
    std::vector<Run> runs;
    uint32_t last_elapsed = 0;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool loss = line.compare(0, 11, "TRACE_LOSS,") == 0;
        if (!loss && line.compare(0, 6, "TRACE,") != 0) continue;
        // A serial capture can stop in the middle of the last record.
        if (input.eof()) {
            if (!runs.empty()) runs.back().capture_has_gaps = true;
            break;
        }

        std::array<std::string_view, 3> fields;
        uint32_t elapsed = 0;
        int16_t free_bytes = 0;
        uint32_t dropped_samples = 0, dropped_events = 0;
        const bool valid = parse_fields(std::string_view(line).substr(loss ? 11 : 6), fields) &&
            parse_number(fields[0], elapsed) &&
            (loss ? parse_number(fields[1], dropped_samples) && parse_number(fields[2], dropped_events) :
                    valid_event_name(fields[1]) && parse_number(fields[2], free_bytes));
        if (!valid) {
            if (!runs.empty()) runs.back().capture_has_gaps = true;
            continue;
        }

        const bool selected = !loss && fields[1] == "selected" && elapsed == 0;
        if (selected || runs.empty()) {
            runs.emplace_back();
            runs.back().printer_status = "incomplete";
            runs.back().capture_has_gaps = !selected;
            last_elapsed = 0;
        }
        auto& run = runs.back();
        if (loss) {
            run.dropped_samples += dropped_samples;
            run.dropped_events += dropped_events;
            run.capture_has_gaps |= dropped_samples != 0 || dropped_events != 0;
            continue;
        }

        if (elapsed < last_elapsed) {
            run.capture_has_gaps = true;
            continue;
        }
        last_elapsed = elapsed;

        const auto name = fields[1];
        if (name == "trace_restarted" && runs.size() > 1)
            runs[runs.size() - 2].capture_has_gaps = true;
        if (name == "finished") run.printer_status = "completed";
        if (name == "cancelled") run.printer_status = "cancelled";
        if (name == "start_failed") run.printer_status = "start_failed";
        run.printer.push_back({std::string(name), elapsed, free_bytes});
    }
    return runs;
}

void write_run(std::ostream& out, const Run& run)
{
    out << "CPAMTRACE1 " << run.id << ' ' << run.started_at_unix_ms << ' ' << std::quoted(run.name)
        << ' ' << std::quoted(run.export_status) << ' ' << std::quoted(run.printer_status)
        << ' ' << run.removable << ' ' << run.capture_has_gaps
        << ' ' << run.dropped_samples << ' ' << run.dropped_events << '\n';
    const auto write_events = [&](char source, const std::vector<Event>& events) {
        for (const auto& event : events) {
            out << source << ' ' << event.elapsed_ms << ' ' << event.name << ' ';
            if (event.free_bytes) out << *event.free_bytes;
            else out << '-';
            out << '\n';
        }
    };
    write_events('S', run.slicer);
    write_events('P', run.printer);
    out << "END\n";
}

std::optional<Run> read_run(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::string token;
    Run run;
    if (!(input >> token) || token != "CPAMTRACE1" ||
        !(input >> run.id >> run.started_at_unix_ms >> std::quoted(run.name) >>
          std::quoted(run.export_status) >> std::quoted(run.printer_status) >>
          run.removable >> run.capture_has_gaps >> run.dropped_samples >> run.dropped_events))
        return {};
    while (input >> token) {
        if (token == "END") return run;
        if (token != "S" && token != "P") return {};
        Event event;
        std::string bytes;
        if (!(input >> event.elapsed_ms >> event.name >> bytes) || !valid_event_name(event.name))
            return {};
        if (bytes != "-") {
            int value;
            if (!parse_number(bytes, value)) return {};
            event.free_bytes = value;
        }
        auto& events = token == "S" ? run.slicer : run.printer;
        if (!events.empty() && event.elapsed_ms < events.back().elapsed_ms) return {};
        events.push_back(std::move(event));
    }
    return {};
}

void attach_printer_run(Run& target, Run&& capture)
{
    target.printer_status = std::move(capture.printer_status);
    target.printer = std::move(capture.printer);
    target.dropped_samples = capture.dropped_samples;
    target.dropped_events = capture.dropped_events;
    target.capture_has_gaps = capture.capture_has_gaps;
}

} // namespace

SaveTime SaveTime::now()
{
    const auto steady = SteadyClock::now();
    const auto unix_ms = std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return {steady, unix_ms.count()};
}

Store::Store(std::filesystem::path directory)
    : m_directory(std::move(directory))
{
    std::random_device random;
    m_next_id = (uint64_t(random()) << 32) | random();
    load();
}

void Store::load()
{
    std::error_code error;
    if (!std::filesystem::exists(m_directory, error) && !error) return;

    std::vector<Run> attachments;
    for (auto it = std::filesystem::directory_iterator(m_directory, error);
         !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
        const auto extension = it->path().extension();
        if (extension != ".trace" && extension != ".printer") continue;
        auto run = read_run(it->path());
        if (!run) {
            m_storage_error = "A print trace could not be read.";
            continue;
        }
        if (extension == ".printer") attachments.push_back(std::move(*run));
        else m_runs.emplace(run->id, std::move(*run));
    }
    if (error) m_storage_error = "Print trace history could not be read.";
    for (auto& attachment : attachments) {
        const auto target = m_runs.find(attachment.id);
        if (target != m_runs.end()) attach_printer_run(target->second, std::move(attachment));
        else m_storage_error = "An associated printer trace has no export.";
    }
}

bool Store::persist(const Run& run, bool attachment)
{
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        m_storage_error = "The print trace directory cannot be written.";
        return false;
    }

    const auto path = m_directory / (std::to_string(run.id) + (attachment ? ".printer" : ".trace"));
    const auto temporary = m_directory / (path.filename().string() + "." + std::to_string(allocate_id()) + ".tmp");
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    write_run(output, run);
    output.close();
    const bool written = output.good();
    if (written) {
        if (attachment)
            std::filesystem::create_hard_link(temporary, path, error);
        else
            std::filesystem::rename(temporary, path, error);
    }
    if (attachment || !written || error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
    }
    if (!written || error) {
        m_storage_error = error == std::errc::file_exists && attachment ?
            "This export already has a printer trace." : "The print trace could not be saved.";
        return false;
    }
    m_storage_error.clear();
    return true;
}

uint64_t Store::allocate_id()
{
    do {
        ++m_next_id;
    } while (m_next_id == 0 || m_runs.count(m_next_id));
    return m_next_id;
}

std::vector<Run> Store::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Run> result;
    result.reserve(m_runs.size());
    for (const auto& row : m_runs) result.push_back(row.second);
    std::sort(result.begin(), result.end(), [](const Run& a, const Run& b) {
        return a.started_at_unix_ms == b.started_at_unix_ms ? a.id > b.id : a.started_at_unix_ms > b.started_at_unix_ms;
    });
    return result;
}

uint64_t Store::revision() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_revision;
}

std::string Store::storage_error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_storage_error;
}

uint64_t Store::begin_export(std::string name, bool removable, SaveTime confirmed)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Run run;
    const auto id = run.id = allocate_id();
    run.name = safe_name(std::move(name));
    run.started_at_unix_ms = confirmed.unix_ms;
    run.removable = removable;
    run.export_status = "exporting";
    run.slicer.push_back({"save_confirmed", 0, {}});
    m_export_starts.emplace(id, confirmed.steady);
    m_runs.emplace(id, std::move(run));
    ++m_revision;
    return id;
}

void Store::record_export(uint64_t id, std::string_view event, SteadyClock::time_point time)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto start = m_export_starts.find(id);
    if (start == m_export_starts.end()) return;
    m_runs.at(id).slicer.push_back({std::string(event), elapsed_ms(start->second, time), {}});
    ++m_revision;
}

void Store::end_export(uint64_t id, std::string_view status, std::string_view event,
                       SteadyClock::time_point time, std::string_view final_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto start = m_export_starts.find(id);
    if (start == m_export_starts.end()) return;
    auto& run = m_runs.at(id);
    run.slicer.push_back({std::string(event), elapsed_ms(start->second, time), {}});
    run.export_status = status;
    if (status == "gcode_saved") run.removable = event == "gcode_on_sd";
    if (!final_name.empty()) run.name = safe_name(std::string(final_name));
    m_export_starts.erase(start);
    // Save outside the measured interval. The terminal event has already captured its timestamp.
    persist(run);
    ++m_revision;
}

ImportResult Store::import_printer_log(std::istream& input, std::string name, std::optional<uint64_t> export_id)
{
    auto captures = parse_capture(input);
    if (input.bad() || (input.fail() && !input.eof())) return {0, "The printer log could not be read."};
    captures.erase(std::remove_if(captures.begin(), captures.end(),
                                  [](const Run& run) { return run.printer.empty(); }), captures.end());
    if (captures.empty()) return {0, "The log contains no printer trace records."};

    const auto imported_at = SaveTime::now().unix_ms;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (export_id) {
        if (captures.size() != 1 || captures.front().printer.front().name != "selected")
            return {0, "Use a log with one print attempt that starts at LCD file selection."};
        const auto target = m_runs.find(*export_id);
        if (target == m_runs.end() || target->second.export_status != "gcode_saved")
            return {0, "Select a saved G-code export."};
        if (!target->second.printer.empty())
            return {0, "This export already has a printer trace."};

        auto& capture = captures.front();
        capture.id = *export_id;
        capture.started_at_unix_ms = imported_at;
        capture.name = safe_name(std::move(name));
        if (!persist(capture, true)) return {0, m_storage_error};
        attach_printer_run(target->second, std::move(capture));
        ++m_revision;
        return {1, {}};
    }

    ImportResult result;
    for (auto& capture : captures) {
        capture.id = allocate_id();
        capture.started_at_unix_ms = imported_at;
        capture.name = safe_name(name);
        if (!persist(capture)) {
            result.error = m_storage_error;
            break;
        }
        m_runs.emplace(capture.id, std::move(capture));
        ++result.imported;
        ++m_revision;
    }
    return result;
}

} // namespace Slic3r::GUI::PrintTrace
