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

constexpr size_t max_live_line_size = 128;

bool is_trace_line(std::string_view line)
{
    return line.compare(0, 6, "TRACE,") == 0 || line.compare(0, 11, "TRACE_LOSS,") == 0;
}

enum class RecordKind { ignored, invalid, event, loss };

struct Record {
    RecordKind kind = RecordKind::ignored;
    Event event;
    uint32_t dropped_samples = 0;
    uint32_t dropped_events = 0;

    bool starts_print() const { return kind == RecordKind::event && event.elapsed_ms == 0 && event.name == "selected"; }
};

Record decode_record(std::string_view line)
{
    Record record;
    if (!is_trace_line(line)) return record;
    record.kind = RecordKind::invalid;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const bool loss = line.compare(0, 11, "TRACE_LOSS,") == 0;
    std::array<std::string_view, 3> fields;
    uint32_t elapsed = 0;
    if (!parse_fields(line.substr(loss ? 11 : 6), fields) || !parse_number(fields[0], elapsed)) return record;
    if (loss) {
        if (parse_number(fields[1], record.dropped_samples) && parse_number(fields[2], record.dropped_events))
            record.kind = RecordKind::loss;
    } else {
        int16_t free_bytes = 0;
        if (valid_event_name(fields[1]) && parse_number(fields[2], free_bytes)) {
            record.kind = RecordKind::event;
            record.event = {std::string(fields[1]), elapsed, free_bytes};
        }
    }
    return record;
}

void apply_record(Run& run, Record&& record)
{
    if (record.kind == RecordKind::loss) {
        run.dropped_samples += record.dropped_samples;
        run.dropped_events += record.dropped_events;
        run.capture_has_gaps |= record.dropped_samples != 0 || record.dropped_events != 0;
    } else if (record.kind == RecordKind::invalid ||
               (!run.printer.empty() && record.event.elapsed_ms < run.printer.back().elapsed_ms)) {
        run.capture_has_gaps = true;
    } else if (record.kind == RecordKind::event) {
        if (record.event.name == "finished") run.printer_status = "completed";
        if (record.event.name == "cancelled") run.printer_status = "cancelled";
        if (record.event.name == "start_failed") run.printer_status = "start_failed";
        run.printer.push_back(std::move(record.event));
    }
}

std::vector<Run> parse_capture(std::istream& input)
{
    std::vector<Run> runs;
    for (std::string line; std::getline(input, line);) {
        if (!is_trace_line(line)) continue;
        // A serial capture can stop in the middle of the last record.
        if (input.eof()) {
            if (!runs.empty()) runs.back().capture_has_gaps = true;
            break;
        }
        auto record = decode_record(line);
        if (runs.empty() && record.kind == RecordKind::invalid) continue;
        const bool selected = record.starts_print();
        if (selected || runs.empty()) {
            runs.emplace_back();
            runs.back().printer_status = "incomplete";
            runs.back().capture_has_gaps = !selected;
        }
        if (record.event.name == "trace_restarted" && runs.size() > 1)
            runs[runs.size() - 2].capture_has_gaps = true;
        apply_record(runs.back(), std::move(record));
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
    if (!(input >> token)) return {};
    if (token == "CPAMLIVE1") {
        if (!(input >> run.id >> run.started_at_unix_ms >> std::quoted(run.name)) || !std::getline(input, token))
            return {};
        auto captures = parse_capture(input);
        if (captures.size() != 1 || captures.front().printer.empty()) return {};
        auto& capture = captures.front();
        capture.id = run.id;
        capture.started_at_unix_ms = run.started_at_unix_ms;
        capture.name = std::move(run.name);
        if (capture.printer_status == "incomplete") {
            capture.printer_status = "interrupted";
            capture.capture_has_gaps = true;
        }
        return std::move(capture);
    }
    if (token != "CPAMTRACE1" ||
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
        if (extension != ".trace" && extension != ".printer" && extension != ".live") continue;
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

bool Store::persist(const Run& run, bool attachment, std::optional<std::string_view> first_live_line)
{
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        m_storage_error = "The print trace directory cannot be written.";
        return false;
    }

    const auto extension = attachment ? ".printer" : (first_live_line ? ".live" : ".trace");
    const auto path = m_directory / (std::to_string(run.id) + extension);
    const auto temporary = m_directory / (path.filename().string() + "." + std::to_string(allocate_id()) + ".tmp");
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (first_live_line)
        output << "CPAMLIVE1 " << run.id << ' ' << run.started_at_unix_ms << ' ' << std::quoted(run.name) << '\n'
               << *first_live_line << '\n';
    else
        write_run(output, run);
    output.close();
    const bool written = output.good();
    const bool exclusive = attachment || first_live_line.has_value();
    if (written) {
        if (exclusive)
            std::filesystem::create_hard_link(temporary, path, error);
        else
            std::filesystem::rename(temporary, path, error);
    }
    if (exclusive || !written || error) {
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

bool Store::track_next_print(std::optional<uint64_t> export_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (export_id) {
        const auto target = m_runs.find(*export_id);
        if (target == m_runs.end() || target->second.export_status != "gcode_saved" || !target->second.printer.empty())
            return false;
    }
    if (m_tracked_export != export_id) {
        m_tracked_export = export_id;
        ++m_revision;
    }
    return true;
}

std::optional<uint64_t> Store::tracked_export() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tracked_export;
}

std::optional<uint64_t> Store::live_run() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_live_id;
}

std::filesystem::path Store::live_path(const Run& run) const
{
    return m_directory / (std::to_string(run.id) + (run.slicer.empty() ? ".live" : ".printer"));
}

void Store::finish_live_run()
{
    if (!m_live_id) return;
    auto& run = m_runs.at(*m_live_id);
    if (run.printer_status == "printing") {
        run.printer_status = "interrupted";
        run.capture_has_gaps = true;
    }
    if (m_live_output.is_open()) {
        m_live_output.close();
        if (!m_live_output) m_storage_error = "The live printer trace could not be saved.";
    }
    m_live_id.reset();
    ++m_revision;
}

void Store::consume_live_line(std::string_view line)
{
    if (line == "start" || line == "start\r") {
        // The MK3 firmware sends this at boot. An armed association must not survive an interrupted print.
        if (m_live_id) m_tracked_export.reset();
        finish_live_run();
        m_previous_live_id.reset();
        return;
    }

    auto record = decode_record(line);
    if (record.kind == RecordKind::ignored) return;
    const bool selected = record.starts_print();
    if (selected || !m_live_id) {
        if (record.kind != RecordKind::event) return;
        m_previous_live_id = m_live_id;
        finish_live_run();
        Run capture;
        capture.name = "USB print";
        capture.started_at_unix_ms = SaveTime::now().unix_ms;
        capture.printer_status = "printing";
        capture.capture_has_gaps = !selected;
        apply_record(capture, std::move(record));

        bool saved = false;
        const auto export_id = selected ? std::exchange(m_tracked_export, std::nullopt) : std::nullopt;
        if (export_id) {
            auto& target = m_runs.at(*export_id);
            std::error_code error;
            const auto export_path = m_directory / (std::to_string(*export_id) + ".trace");
            if (target.printer.empty() && std::filesystem::is_regular_file(export_path, error)) {
                capture.id = *export_id;
                saved = persist(capture, true, line);
                if (saved) attach_printer_run(target, std::move(capture));
            }
        }
        if (!saved) {
            const auto id = capture.id = allocate_id();
            saved = persist(capture, false, line);
            m_runs.emplace(id, std::move(capture));
            m_live_id = id;
        } else {
            m_live_id = export_id;
        }
        if (saved) {
            m_live_output.open(live_path(m_runs.at(*m_live_id)), std::ios::out | std::ios::app);
            if (!m_live_output) m_storage_error = "The live printer trace could not be opened.";
        }
        ++m_revision;
        return;
    }

    if (record.event.name == "trace_restarted" && m_previous_live_id) {
        auto& previous = m_runs.at(*m_previous_live_id);
        previous.capture_has_gaps = true;
        std::error_code error;
        const auto path = live_path(previous);
        if (std::filesystem::is_regular_file(path, error)) {
            std::ofstream output(path, std::ios::out | std::ios::app);
            output << "TRACE,\n";
            output.close();
            if (!output) m_storage_error = "The live printer trace could not be saved.";
        }
        m_previous_live_id.reset();
    }
    apply_record(m_runs.at(*m_live_id), std::move(record));
    if (m_live_output.is_open()) m_live_output << line << '\n';
    ++m_revision;
}

void Store::append_live(std::string_view bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto previous_revision = m_revision;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        const auto fragment = bytes.substr(0, newline);
        const auto space = max_live_line_size - m_live_line.size();
        m_live_line.append(fragment.substr(0, space));
        m_live_line_too_long |= fragment.size() > space;
        if (newline == std::string_view::npos) break;
        if (!m_live_line_too_long)
            consume_live_line(m_live_line);
        else if (is_trace_line(m_live_line))
            consume_live_line("TRACE,");
        m_live_line.clear();
        m_live_line_too_long = false;
        bytes.remove_prefix(newline + 1);
    }
    // Each USB read can contain multiple records. Flush the batch without rewriting the run.
    if (m_revision != previous_revision && m_live_output.is_open()) {
        m_live_output.flush();
        if (!m_live_output) m_storage_error = "The live printer trace could not be saved.";
    }
}

void Store::end_live_capture()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (is_trace_line(m_live_line)) consume_live_line("TRACE,");
    m_live_line.clear();
    m_live_line_too_long = false;
    if (m_tracked_export) {
        m_tracked_export.reset();
        ++m_revision;
    }
    finish_live_run();
    m_previous_live_id.reset();
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
