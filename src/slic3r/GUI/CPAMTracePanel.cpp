// Released under the terms of the AGPLv3 or higher.
#include "I18N.hpp"
#include "CPAMTracePanel.hpp"

#include "CPAMTracePlot.hpp"
#include "PrintTrace.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/nowide/fstream.hpp>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace Slic3r::GUI {
namespace {

using PrintTrace::Event;
using PrintTrace::Run;
using CPAMTracePlot::RamSample;
using Millis = std::uint64_t;

wxString number(std::uint64_t value)
{
    return wxString::Format("%llu", static_cast<unsigned long long>(value));
}

wxString elapsed(Millis ms)
{
    const auto fraction = static_cast<unsigned>(ms % 1000);
    if (ms < 60000) return wxString::Format("%u.%03u s", static_cast<unsigned>(ms / 1000), fraction);
    return wxString::Format("%s:%02u.%03u", number(ms / 60000), static_cast<unsigned>(ms / 1000 % 60), fraction);
}

wxString local_time(std::int64_t unix_ms, bool include_date)
{
    const wxDateTime date(static_cast<std::time_t>(unix_ms / 1000));
    if (!date.IsValid()) return {};
    if (include_date) return date.FormatISOCombined(' ');
    return wxString::Format("%s.%03u", date.FormatISOTime(), static_cast<unsigned>(unix_ms % 1000));
}

wxString event_label(const std::string &name)
{
    if (name == "save_confirmed") return _L("Save confirmed");
    if (name == "gcode_ready") return _L("Final G-code ready");
    if (name == "sd_write_started") return _L("SD write started");
    if (name == "local_write_started") return _L("Local write started");
    if (name == "gcode_on_sd") return _L("G-code on SD");
    if (name == "gcode_saved_local") return _L("G-code saved locally");
    if (name == "export_failed") return _L("Export failed");
    if (name == "export_cancelled") return _L("Export cancelled");
    if (name == "selected") return _L("SD file accepted");
    if (name == "sd_check_start") return _L("File check started");
    if (name == "sd_check_ok") return _L("File check passed");
    if (name == "sd_check_incomplete") return _L("File check incomplete");
    if (name == "sd_started") return _L("SD file opened");
    if (name == "first_gcode") return _L("First G-code command");
    if (name == "first_layer_marker") return _L("First layer marker");
    if (name == "finished") return _L("Print complete");
    if (name == "cancelled") return _L("Print cancelled");
    if (name == "start_failed") return _L("Print start failed");
    if (name == "trace_restarted") return _L("Previous trace interrupted");
    return wxString::FromUTF8(name);
}

wxString run_status(const Run &run)
{
    if (run.printer_status == "completed") return _L("Completed");
    if (run.printer_status == "cancelled") return _L("Print cancelled");
    if (run.printer_status == "start_failed") return _L("Start failed");
    if (run.printer_status == "incomplete") return _L("Capture incomplete");
    if (run.export_status == "gcode_saved") return run.removable ? _L("SD export only") : _L("Local export only");
    if (run.export_status == "exporting") return _L("Export in progress");
    if (run.export_status == "failed") return _L("Export failed");
    if (run.export_status == "cancelled") return _L("Export cancelled");
    if (run.export_status == "interrupted") return _L("Export interrupted");
    return _L("Printer capture");
}

const Event *find_event(const std::vector<Event> &events, std::initializer_list<const char*> names)
{
    auto found = std::find_if(events.begin(), events.end(), [names](const Event &event) {
        return std::any_of(names.begin(), names.end(), [&event](const char *name) { return event.name == name; });
    });
    return found == events.end() ? nullptr : &*found;
}

struct Span
{
    wxString label;
    Millis start;
    Millis end;
};

void add_span(std::vector<Span> &spans, const wxString &label, const Event *begin, const Event *end)
{
    if (begin && end && end->elapsed_ms >= begin->elapsed_ms)
        spans.push_back({label, begin->elapsed_ms, end->elapsed_ms});
}

wxStaticText *heading(wxWindow *parent, const wxString &text, bool prominent = false)
{
    auto *label = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
        prominent ? wxST_ELLIPSIZE_END : 0);
    wxFont font = label->GetFont();
    font.MakeBold();
    if (prominent) font.SetPointSize(font.GetPointSize() + 3);
    label->SetFont(font);
    if (prominent) label->SetMinSize(wxSize(parent->FromDIP(160), -1));
    return label;
}

class TraceChart : public wxPanel
{
public:
    explicit TraceChart(wxWindow *parent) : wxPanel(parent, wxID_ANY)
    {
        SetMinSize(FromDIP(wxSize(235, 128)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &TraceChart::paint, this);
    }

    void set_data(Millis end, std::vector<Span> spans, const std::vector<RamSample> *samples,
        const wxString &empty, const Event *final_event = nullptr)
    {
        m_end = std::max<Millis>(1, end);
        m_spans = std::move(spans);
        m_samples = samples;
        m_empty = empty;
        m_final = final_event ? std::make_optional(final_event->elapsed_ms) : std::nullopt;
        m_final_label = final_event ? event_label(final_event->name) : wxString();
        Refresh();
    }

private:
    void paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        dc.Clear();
        dc.SetFont(GetFont().Smaller());

        const auto size = GetClientSize();
        const int left = FromDIP(130), right = size.x - FromDIP(25);
        const int top = FromDIP(12), bottom = size.y - FromDIP(24);
        if (right <= left || bottom <= top) return;
        if (m_spans.empty() && (!m_samples || m_samples->empty()) && !m_final) {
            dc.DrawText(m_empty, FromDIP(10), FromDIP(12));
            return;
        }

        const wxColour grid = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW);
        const wxColour accent(204, 83, 28);
        const auto x_at = [&](Millis ms) {
            return left + static_cast<int>(static_cast<long double>(std::min(ms, m_end)) / m_end * (right - left));
        };
        const unsigned divisions = right - left < FromDIP(190) ? 2 : 4;
        dc.SetPen(wxPen(grid));
        for (unsigned step = 0; step <= divisions; ++step) {
            const Millis ms = m_end / divisions * step + m_end % divisions * step / divisions;
            const int x = x_at(ms);
            dc.DrawLine(x, top, x, bottom);
            const auto label = step == 0 ? wxString("0") : elapsed(ms);
            const auto width = dc.GetTextExtent(label).x;
            dc.DrawText(label, std::min(x - width / 2, size.x - width - FromDIP(2)), bottom + FromDIP(4));
        }

        if (m_samples) {
            const auto points = CPAMTracePlot::ram_points(*m_samples, m_end, right - left + 1);
            if (points.empty()) return;
            int min_free = points.front().sample.free_bytes, max_free = min_free;
            for (const auto &point : points) {
                min_free = std::min(min_free, point.sample.free_bytes);
                max_free = std::max(max_free, point.sample.free_bytes);
            }
            const auto low = static_cast<std::int64_t>(min_free) - 10;
            const auto high = static_cast<std::int64_t>(max_free) + 10;
            const auto y_at = [&](std::int64_t bytes) {
                return bottom - static_cast<int>(static_cast<double>(bytes - low) / (high - low) * (bottom - top));
            };
            for (const auto value : {low, low + (high - low) / 2, high}) {
                const int y = y_at(value);
                dc.DrawLine(left, y, right, y);
                const auto label = wxString::Format("%lld B", static_cast<long long>(value));
                dc.DrawText(label, left - dc.GetTextExtent(label).x - FromDIP(6),
                    std::max(0, y - dc.GetCharHeight() / 2));
            }
            dc.SetPen(wxPen(accent, FromDIP(2)));
            dc.SetBrush(wxBrush(accent));
            wxPoint previous;
            for (const auto &point : points) {
                const wxPoint current(x_at(point.sample.ms), y_at(point.sample.free_bytes));
                if (point.new_segment) dc.DrawCircle(current, FromDIP(2));
                else dc.DrawLine(previous, current);
                previous = current;
            }
        } else {
            int y = top + FromDIP(12);
            for (const auto &span : m_spans) {
                if (span.start > m_end || y > bottom) continue;
                dc.SetClippingRegion(0, 0, left - FromDIP(4), bottom);
                dc.DrawText(span.label, FromDIP(5), y - dc.GetCharHeight() / 2);
                dc.DestroyClippingRegion();
                dc.SetPen(wxPen(accent, FromDIP(5)));
                dc.DrawLine(x_at(span.start), y, std::max(x_at(span.start) + 1, x_at(span.end)), y);
                y += FromDIP(23);
            }
        }
        dc.SetPen(wxPen(grid));
        dc.DrawLine(left, bottom, right, bottom);
        if (m_final && *m_final <= m_end) {
            const int x = x_at(*m_final);
            dc.SetPen(wxPen(accent));
            dc.DrawLine(x, top, x, bottom);
            const int width = dc.GetTextExtent(m_final_label).x;
            dc.DrawText(m_final_label, std::max(left, x - width - FromDIP(4)), bottom - dc.GetCharHeight());
        }
    }

    Millis m_end{1};
    std::vector<Span> m_spans;
    const std::vector<RamSample> *m_samples{nullptr};
    wxString m_empty;
    std::optional<Millis> m_final;
    wxString m_final_label;
};

class RamTable : public wxListCtrl
{
public:
    explicit RamTable(wxWindow *parent)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
    {
        AppendColumn(_L("Elapsed"), wxLIST_FORMAT_RIGHT, FromDIP(108));
        AppendColumn(_L("Free SRAM"), wxLIST_FORMAT_RIGHT, FromDIP(105));
    }

    void show_samples(const std::vector<RamSample> &samples, Millis end)
    {
        m_samples = &samples;
        const auto until = std::upper_bound(samples.begin(), samples.end(), end,
            [](Millis time, const RamSample &sample) { return time < sample.ms; });
        const auto size = static_cast<long>(std::distance(samples.begin(), until));
        SetItemCount(size);
        if (size > 0) RefreshItems(0, size - 1);
    }

protected:
    wxString OnGetItemText(long item, long col) const override
    {
        if (!m_samples || item < 0 || static_cast<std::size_t>(item) >= m_samples->size()) return {};
        const auto &sample = (*m_samples)[item];
        return col == 0 ? elapsed(sample.ms) : wxString::Format("%d B", sample.free_bytes);
    }

private:
    const std::vector<RamSample> *m_samples{nullptr};
};

struct Section
{
    Section(wxWindow *parent, wxBoxSizer *layout, const wxString &title, const wxString &caption, bool memory = false)
    {
        layout->Add(new wxStaticLine(parent), 0, wxEXPAND | wxTOP, parent->FromDIP(8));
        auto *labels = new wxBoxSizer(wxHORIZONTAL);
        auto *titles = new wxBoxSizer(wxVERTICAL);
        titles->Add(heading(parent, title));
        titles->Add(new wxStaticText(parent, wxID_ANY, caption));
        labels->Add(titles, 1, wxEXPAND | wxRIGHT, parent->FromDIP(6));
        summary = new wxStaticText(parent, wxID_ANY, wxEmptyString);
        labels->Add(summary, 0, wxALIGN_CENTER_VERTICAL);
        layout->Add(labels, 0, wxEXPAND | wxTOP | wxBOTTOM, parent->FromDIP(8));

        auto *body = new wxBoxSizer(wxHORIZONTAL);
        chart = new TraceChart(parent);
        table = memory ? static_cast<wxListCtrl*>(new RamTable(parent))
                       : new wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        table->SetMinSize(parent->FromDIP(wxSize(300, 128)));
        body->Add(chart, 1, wxEXPAND | wxRIGHT, parent->FromDIP(10));
        body->Add(table, 1, wxEXPAND);
        layout->Add(body, 1, wxEXPAND);
    }

    TraceChart *chart;
    wxListCtrl *table;
    wxStaticText *summary;
};

void insert_event(wxListCtrl &table, const Event &event, const wxString &detail)
{
    const auto row = table.InsertItem(table.GetItemCount(), event_label(event.name));
    table.SetItem(row, 1, elapsed(event.elapsed_ms));
    table.SetItem(row, 2, detail);
}

} // namespace

struct CPAMTracePanel::Impl
{
    explicit Impl(CPAMTracePanel &owner)
        : owner(owner), timer(&owner)
    {
        const int margin = owner.FromDIP(12);
        auto *content = new wxBoxSizer(wxHORIZONTAL);

        auto *side = new wxPanel(&owner);
        side->SetMinSize(owner.FromDIP(wxSize(270, -1)));
        auto *side_layout = new wxBoxSizer(wxVERTICAL);
        side_layout->Add(heading(side, _L("Print history")), 0, wxALL, margin);
        filter = new wxSearchCtrl(side, wxID_ANY);
        filter->SetDescriptiveText(_L("Find a run"));
        side_layout->Add(filter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margin);
        history = new wxListCtrl(side, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        history->AppendColumn(_L("File"), wxLIST_FORMAT_LEFT, owner.FromDIP(137));
        history->AppendColumn(_L("Status"), wxLIST_FORMAT_LEFT, owner.FromDIP(108));
        side_layout->Add(history, 1, wxEXPAND | wxBOTTOM | wxLEFT | wxRIGHT, margin);
        side->SetSizer(side_layout);
        content->Add(side, 0, wxEXPAND);
        content->Add(new wxStaticLine(&owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND);

        detail = new wxScrolledWindow(&owner, wxID_ANY);
        detail->SetScrollRate(margin, margin);
        detail->SetMinSize(owner.FromDIP(wxSize(360, 280)));
        auto *layout = new wxBoxSizer(wxVERTICAL);
        auto *toolbar = new wxBoxSizer(wxHORIZONTAL);
        auto *import_button = new wxButton(detail, wxID_ANY, _L("Import printer log…"));
        export_button = new wxButton(detail, wxID_ANY, _L("Export CSV…"));
        range = new wxChoice(detail, wxID_ANY);
        range->Append(_L("Startup"));
        range->Append(_L("Full print"));
        range->SetSelection(0);
        toolbar->Add(import_button, 0, wxRIGHT, owner.FromDIP(7));
        toolbar->Add(export_button, 0);
        toolbar->AddStretchSpacer();
        toolbar->Add(range, 0);
        layout->Add(toolbar, 0, wxEXPAND | wxBOTTOM, margin);

        selected_name = heading(detail, _L("No runs yet"), true);
        selected_status = new wxStaticText(detail, wxID_ANY, wxEmptyString);
        notice = new wxStaticText(detail, wxID_ANY, wxEmptyString);
        layout->Add(selected_name, 0, wxEXPAND | wxBOTTOM, owner.FromDIP(3));
        layout->Add(selected_status, 0, wxEXPAND);
        layout->Add(notice, 0, wxEXPAND | wxTOP, owner.FromDIP(3));

        slicer.emplace(detail, layout, _L("Slicer latency"), _L("Clock starts when you confirm Save."));
        memory.emplace(detail, layout, _L("Printer memory"), _L("Free SRAM, 100 ms target interval"), true);
        printer.emplace(detail, layout, _L("Printer event latency"), _L("Clock starts when the LCD accepts the SD file."));
        const auto event_columns = [&](wxListCtrl &table, const wxString &last) {
            table.AppendColumn(_L("Event"), wxLIST_FORMAT_LEFT, owner.FromDIP(137));
            table.AppendColumn(_L("Elapsed"), wxLIST_FORMAT_RIGHT, owner.FromDIP(72));
            table.AppendColumn(last, wxLIST_FORMAT_RIGHT, owner.FromDIP(86));
        };
        event_columns(*slicer->table, _L("Local time"));
        event_columns(*printer->table, _L("Duration"));

        auto *padded = new wxBoxSizer(wxVERTICAL);
        padded->Add(layout, 1, wxEXPAND | wxALL, margin);
        detail->SetSizer(padded);
        content->Add(detail, 1, wxEXPAND);
        owner.SetSizer(content);

        filter->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { rebuild_history(); });
        history->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &event) {
            const auto index = static_cast<std::size_t>(event.GetIndex());
            if (rebuilding || index >= visible.size() || selected == runs[visible[index]].id) return;
            selected = runs[visible[index]].id;
            range->SetSelection(0);
            show_run();
        });
        range->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { show_run(); });
        import_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { import_log(); });
        export_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { export_csv(); });
        owner.Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
            if (this->owner.IsShownOnScreen()) this->owner.refresh();
        }, timer.GetId());
        timer.Start(750);
        show_run();
    }

    const Run *find_run(std::optional<std::uint64_t> id) const
    {
        const auto it = std::find_if(runs.begin(), runs.end(), [id](const Run &run) { return run.id == id; });
        return it == runs.end() ? nullptr : &*it;
    }

    void rebuild_history()
    {
        rebuilding = true;
        history->Freeze();
        history->DeleteAllItems();
        visible.clear();
        const auto query = filter->GetValue().Lower();
        long selection = -1;
        for (std::size_t index = 0; index < runs.size(); ++index) {
            const Run &run = runs[index];
            const auto name = wxString::FromUTF8(run.name);
            if (!query.empty() && !name.Lower().Contains(query) &&
                !number(run.id).Contains(query) && !run_status(run).Lower().Contains(query)) continue;
            const auto item = history->InsertItem(history->GetItemCount(), name);
            history->SetItem(item, 1, run_status(run));
            visible.push_back(index);
            if (run.id == selected) selection = item;
        }
        if (selection == -1 && !visible.empty()) {
            selection = 0;
            selected = runs[visible.front()].id;
            range->SetSelection(0);
        }
        if (selection >= 0) {
            history->SetItemState(selection, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            history->EnsureVisible(selection);
        } else {
            selected.reset();
        }
        history->Thaw();
        rebuilding = false;
        show_run();
    }

    void show_run()
    {
        const Run *run = find_run(selected);
        selected_name->SetLabelText(run ? wxString::FromUTF8(run->name) : _L("Select a run"));
        selected_status->SetLabelText(run ? run_status(*run) + "   ·   #" + number(run->id) + "   ·   " +
            (run->slicer.empty() ? _L("Imported") : _L("Started")) + " " +
            local_time(run->started_at_unix_ms, true) :
            _L("Export G-code or import a printer log to start the history."));
        const auto storage_error = revision ? PrintTrace::store().storage_error() : std::string();
        if (!storage_error.empty()) {
            notice->SetLabelText(wxString::FromUTF8(storage_error));
        } else if (run && run->capture_has_gaps) {
            notice->SetLabelText(_L("Capture has gaps") + "   ·   " + _L("Dropped RAM samples") + " " +
                number(run->dropped_samples) + "   ·   " + _L("Dropped events") + " " + number(run->dropped_events));
        } else {
            notice->SetLabelText(wxEmptyString);
        }
        notice->Show(!notice->GetLabel().empty());
        export_button->Enable(run != nullptr);
        range->Enable(run && !run->printer.empty());

        slicer->table->DeleteAllItems();
        printer->table->DeleteAllItems();
        std::vector<Span> export_spans, printer_spans;
        Millis export_end = 1, printer_end = 1;
        const Event *final = nullptr;
        samples.clear();
        if (run && !run->slicer.empty()) {
            const auto &events = run->slicer;
            export_end = events.back().elapsed_ms;
            const Event *saved = find_event(events, {"gcode_on_sd", "gcode_saved_local"});
            add_span(export_spans, _L("Prepare G-code"), &events.front(), find_event(events, {"gcode_ready"}));
            add_span(export_spans, run->removable ? _L("Write to SD") : _L("Save to disk"),
                find_event(events, {"sd_write_started", "local_write_started"}), saved);
            for (const Event &event : events)
                insert_event(*slicer->table, event,
                    local_time(run->started_at_unix_ms + static_cast<std::int64_t>(event.elapsed_ms), false));
            slicer->summary->SetLabelText(saved ? _L("Save to file ready") + "  " + elapsed(saved->elapsed_ms) : wxString());
        } else {
            slicer->summary->SetLabelText(wxEmptyString);
        }

        if (run && !run->printer.empty()) {
            const auto &events = run->printer;
            const Event *check_begin = find_event(events, {"sd_check_start"});
            const Event *check_end = find_event(events, {"sd_check_ok", "sd_check_incomplete"});
            const Event *started = find_event(events, {"sd_started"});
            const Event *command = find_event(events, {"first_gcode"});
            final = find_event(events, {"finished", "cancelled", "start_failed"});
            printer_end = std::max<Millis>(1, events.back().elapsed_ms);
            if (range->GetSelection() == 0) {
                const Event *startup = command ? command : (started ? started : check_end);
                printer_end = CPAMTracePlot::startup_end(printer_end, startup ? startup->elapsed_ms : 0);
            }
            add_span(printer_spans, _L("File check"), check_begin, check_end);
            add_span(printer_spans, _L("Open to first G-code"), started, command);
            if (range->GetSelection() == 1) add_span(printer_spans, _L("Print"), command, final);
            for (const Event &event : events) {
                if ((event.name == "selected" || event.name == "ram_sample") && event.free_bytes)
                    samples.push_back({event.elapsed_ms, *event.free_bytes});
                if (event.name == "ram_sample") continue;
                wxString duration;
                if (&event == check_end && check_begin && event.elapsed_ms >= check_begin->elapsed_ms)
                    duration = elapsed(event.elapsed_ms - check_begin->elapsed_ms);
                if (&event == command && started && event.elapsed_ms >= started->elapsed_ms)
                    duration = elapsed(event.elapsed_ms - started->elapsed_ms);
                if (&event == final && command && event.elapsed_ms >= command->elapsed_ms)
                    duration = elapsed(event.elapsed_ms - command->elapsed_ms);
                insert_event(*printer->table, event, duration);
            }
        }

        std::optional<int> min_free;
        for (const RamSample &sample : samples) {
            if (sample.ms > printer_end) break;
            if (!min_free || sample.free_bytes < *min_free) min_free = sample.free_bytes;
        }
        memory->summary->SetLabelText(min_free ? wxString::Format("%s  %d B", _L("Minimum in view"), *min_free) : wxString());
        printer->summary->SetLabelText(final ? event_label(final->name) + "  " + elapsed(final->elapsed_ms) : wxString());
        memory->chart->set_data(printer_end, {}, min_free ? &samples : nullptr, _L("No RAM samples in this view."));
        static_cast<RamTable*>(memory->table)->show_samples(samples, printer_end);
        slicer->chart->set_data(export_end, std::move(export_spans), nullptr, _L("No completed export phases."));
        printer->chart->set_data(printer_end, std::move(printer_spans), nullptr, _L("No completed printer phases."), final);
        detail->Layout();
        detail->FitInside();
    }

    void import_log()
    {
        const auto prior_selection = selected;
        wxFileDialog chooser(&owner, _L("Import printer log"), wxEmptyString, wxEmptyString, _L("All files (*.*)|*.*"),
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (chooser.ShowModal() != wxID_OK) return;
        const Run *run = find_run(prior_selection);
        std::optional<std::uint64_t> link;
        if (run && run->export_status == "gcode_saved" && run->printer.empty()) {
            const std::uint64_t candidate_id = run->id;
            wxMessageDialog ask(&owner, _L("Link this capture to the selected export?") + "\n\n" +
                wxString::FromUTF8(run->name), _L("Import printer log"), wxYES_NO | wxCANCEL | wxNO_DEFAULT);
            ask.SetYesNoLabels(_L("Link to selected export"), _L("Import as a new run"));
            const int choice = ask.ShowModal();
            if (choice == wxID_CANCEL) return;
            if (choice == wxID_YES) link = candidate_id;
        }
        const auto path = chooser.GetPath().ToUTF8();
        boost::nowide::ifstream input(path.data(), std::ios::binary);
        if (!input) {
            wxMessageBox(_L("The printer log could not be opened."), _L("Import printer log"), wxOK | wxICON_ERROR, &owner);
            return;
        }
        const auto result = PrintTrace::store().import_printer_log(input, chooser.GetFilename().ToStdString(wxConvUTF8), link);
        if (!result.error.empty())
            wxMessageBox(wxString::FromUTF8(result.error), _L("Import printer log"), wxOK | wxICON_ERROR, &owner);
        if (result.imported > 0 && !link) {
            selected.reset();
            filter->ChangeValue(wxEmptyString);
        }
        owner.refresh();
    }

    void export_csv()
    {
        const auto id = selected;
        if (!id) return;
        wxFileDialog chooser(&owner, _L("Export trace"), wxEmptyString, "trace-" + number(*id) + ".csv",
            _L("CSV files (*.csv)|*.csv"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (chooser.ShowModal() != wxID_OK) return;
        const Run *run = find_run(id);
        if (!run) return;
        const auto path = chooser.GetPath().ToUTF8();
        boost::nowide::ofstream output(path.data(), std::ios::binary);
        output << "run_id,clock,event,elapsed_ms,free_sram_bytes\r\n";
        const auto write = [&](const std::vector<Event> &events, const char *clock) {
            for (const Event &event : events) {
                output << run->id << ',' << clock << ",\"";
                for (const char character : event.name) {
                    if (character == '"') output << '"';
                    output << character;
                }
                output << "\"," << event.elapsed_ms << ',';
                if (event.free_bytes) output << *event.free_bytes;
                output << "\r\n";
            }
        };
        write(run->slicer, "slicer");
        write(run->printer, "printer");
        output.close();
        if (!output)
            wxMessageBox(_L("The CSV file could not be saved."), _L("Export trace"), wxOK | wxICON_ERROR, &owner);
    }

    CPAMTracePanel &owner;
    wxTimer timer;
    std::optional<std::uint64_t> revision;
    std::optional<std::uint64_t> selected;
    std::vector<Run> runs;
    std::vector<std::size_t> visible;
    std::vector<RamSample> samples;
    bool rebuilding{false};
    wxScrolledWindow *detail;
    wxSearchCtrl *filter;
    wxListCtrl *history;
    wxStaticText *selected_name;
    wxStaticText *selected_status;
    wxStaticText *notice;
    wxButton *export_button;
    wxChoice *range;
    std::optional<Section> slicer;
    std::optional<Section> memory;
    std::optional<Section> printer;
};

CPAMTracePanel::CPAMTracePanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY), m_impl(std::make_unique<Impl>(*this))
{
}

CPAMTracePanel::~CPAMTracePanel()
{
    m_impl->timer.Stop();
    // The virtual RAM table refers to data owned by the implementation.
    DestroyChildren();
}

void CPAMTracePanel::refresh()
{
    auto &store = PrintTrace::store();
    const auto revision = store.revision();
    if (m_impl->revision == revision) return;
    m_impl->revision = revision;
    m_impl->runs = store.snapshot();
    m_impl->rebuild_history();
}

} // namespace Slic3r::GUI
