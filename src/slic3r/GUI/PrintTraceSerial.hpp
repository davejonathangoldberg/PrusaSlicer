// Released under the terms of the AGPLv3 or higher.
#ifndef slic3r_GUI_PrintTraceSerial_hpp_
#define slic3r_GUI_PrintTraceSerial_hpp_

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace Slic3r::GUI::PrintTrace {

class SerialReader
{
public:
    using BytesCallback = std::function<void(std::string_view)>;
    using DisconnectCallback = std::function<void(std::error_code)>;

    SerialReader() = default;
    ~SerialReader();
    SerialReader(const SerialReader&) = delete;
    SerialReader& operator=(const SerialReader&) = delete;

    static bool supported() noexcept;
    bool connected() const noexcept { return m_connected.load(); }

    // Control the reader from its owner thread. Callbacks run on the serial
    // thread and must not call start() or stop(). Copy any bytes needed after
    // the callback returns. Explicit stop joins without a disconnect callback.
    std::error_code start(const std::string& port, BytesCallback on_bytes, DisconnectCallback on_disconnect);
    void stop();

private:
    std::atomic_bool m_connected{false};
    std::atomic_bool m_stopping{false};
    int m_stop_read{-1};
    int m_stop_write{-1};
    std::thread m_worker;
};

} // namespace Slic3r::GUI::PrintTrace

#endif
