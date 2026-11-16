// Released under the terms of the AGPLv3 or higher.
#include "PrintTraceSerial.hpp"

#if defined(__APPLE__) || defined(__linux__)
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#endif

namespace Slic3r::GUI::PrintTrace {

#if defined(__APPLE__) || defined(__linux__)
namespace {

std::error_code serial_error()
{
    return {errno, std::generic_category()};
}

std::error_code configure(int fd)
{
    termios settings{};
    if (::tcgetattr(fd, &settings) < 0)
        return serial_error();

    ::cfmakeraw(&settings);
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    settings.c_cflag &= ~(CSTOPB | HUPCL);
#ifdef CRTSCTS
    settings.c_cflag &= ~CRTSCTS;
#endif
#ifdef __APPLE__
    settings.c_cflag &= ~(CDTR_IFLOW | CDSR_OFLOW | CCAR_OFLOW);
#endif
    settings.c_cflag |= CLOCAL | CREAD;
    if (::cfsetispeed(&settings, B115200) < 0 || ::cfsetospeed(&settings, B115200) < 0 ||
        ::tcsetattr(fd, TCSANOW, &settings) < 0)
        return serial_error();

    return {};
}

std::error_code read_stream(int fd, int stop_read, const SerialReader::BytesCallback& on_bytes)
{
    std::array<char, 8192> buffer;
    pollfd input[] = {{fd, POLLIN, 0}, {stop_read, POLLIN, 0}};
    while (true) {
        const int ready = ::poll(input, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return serial_error();
        }
        if (input[1].revents)
            return {};

        if (input[0].revents & POLLIN) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count > 0) {
                on_bytes(std::string_view(buffer.data(), static_cast<size_t>(count)));
                continue;
            }
            if (count == 0)
                return std::make_error_code(std::errc::not_connected);
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return serial_error();
        }
        if (input[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::make_error_code(std::errc::not_connected);
    }
}

} // namespace
#endif

SerialReader::~SerialReader()
{
    stop();
}

bool SerialReader::supported() noexcept
{
#if defined(__APPLE__) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

std::error_code SerialReader::start(const std::string& port, BytesCallback on_bytes, DisconnectCallback on_disconnect)
{
#if defined(__APPLE__) || defined(__linux__)
    if (connected())
        return std::make_error_code(std::errc::device_or_resource_busy);
    if (port.empty() || !on_bytes || !on_disconnect)
        return std::make_error_code(std::errc::invalid_argument);
    stop();

    const int fd = ::open(port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return serial_error();
    if (const auto error = configure(fd)) {
        ::close(fd);
        return error;
    }

    int stop_pipe[2];
    if (::pipe(stop_pipe) < 0) {
        const auto error = serial_error();
        ::close(fd);
        return error;
    }
    m_stop_read = stop_pipe[0];
    m_stop_write = stop_pipe[1];
    if (::fcntl(m_stop_read, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(m_stop_write, F_SETFD, FD_CLOEXEC) < 0) {
        const auto error = serial_error();
        ::close(fd);
        stop();
        return error;
    }

    m_stopping.store(false);
    m_connected.store(true);
    try {
        m_worker = std::thread([this, fd, stop_read = m_stop_read,
                               on_bytes = std::move(on_bytes), on_disconnect = std::move(on_disconnect)] {
            std::error_code error;
            try {
                error = read_stream(fd, stop_read, on_bytes);
            } catch (...) {
                error = std::make_error_code(std::errc::io_error);
            }

            ::close(fd);
            m_connected.store(false);
            if (error && !m_stopping.load()) {
                try {
                    on_disconnect(error);
                } catch (...) {
                    // The port is closed even if the receiver cannot report the error.
                }
            }
        });
    } catch (const std::system_error& error) {
        ::close(fd);
        m_connected.store(false);
        stop();
        return error.code();
    } catch (...) {
        ::close(fd);
        m_connected.store(false);
        stop();
        throw;
    }
    return {};
#else
    (void)port;
    (void)on_bytes;
    (void)on_disconnect;
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}

void SerialReader::stop()
{
    m_stopping.store(true);
#if defined(__APPLE__) || defined(__linux__)
    // Closing the write end wakes poll even when the printer sends no data.
    if (m_stop_write >= 0)
        ::close(std::exchange(m_stop_write, -1));
#endif
    if (m_worker.joinable())
        m_worker.join();
#if defined(__APPLE__) || defined(__linux__)
    if (m_stop_read >= 0)
        ::close(std::exchange(m_stop_read, -1));
#endif
}

} // namespace Slic3r::GUI::PrintTrace
