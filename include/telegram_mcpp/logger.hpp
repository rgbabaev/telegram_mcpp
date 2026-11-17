#pragma once

#include <chrono>
#include <cstddef>
#include <iosfwd>
#include <mutex>

namespace telegram_mcpp {

enum class Event {
    config_created, config_validated, command_failed, authorization_changed,
    telegram_unavailable, connection_changed, shutdown_failed
};

class Logger {
public:
    explicit Logger(std::ostream& output);
    void write(Event event);
    void request_completed(std::chrono::milliseconds duration, std::size_t pending);

private:
    std::ostream& output_;
    std::mutex mutex_;
};

}
