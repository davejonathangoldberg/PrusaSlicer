// Released under the terms of the AGPLv3 or higher.
#include "PrintTrace.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r::GUI::PrintTrace {

Store& store()
{
    static Store history(std::filesystem::u8path(Slic3r::data_dir()) / "print-traces");
    return history;
}

uint64_t begin_export(std::string_view path, bool removable, SaveTime confirmed) noexcept
{
    try {
        return store().begin_export(std::filesystem::u8path(path).filename().u8string(), removable, confirmed);
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot start print trace: " << error.what();
        return 0;
    }
}

void record_export(uint64_t id, std::string_view event) noexcept
{
    if (id == 0) return;
    const auto time = SteadyClock::now();
    try {
        store().record_export(id, event, time);
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot record print trace: " << error.what();
    }
}

void end_export(uint64_t id, std::string_view status, std::string_view event, std::string_view final_path) noexcept
{
    if (id == 0) return;
    const auto time = SteadyClock::now();
    try {
        const auto filename = std::filesystem::u8path(final_path).filename().u8string();
        store().end_export(id, status, event, time, filename);
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot finish print trace: " << error.what();
    }
}

} // namespace Slic3r::GUI::PrintTrace
