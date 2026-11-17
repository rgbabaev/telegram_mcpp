#include "telegram_mcpp/logger.hpp"

#include <ostream>
#include <string_view>

namespace telegram_mcpp {
namespace {

std::string_view event_name(Event event) {
    switch (event) {
    case Event::config_created: return "info config_created";
    case Event::config_validated: return "info config_validated";
    case Event::authorization_changed: return "info authorization_changed";
    case Event::telegram_unavailable: return "error telegram_unavailable";
    case Event::connection_changed: return "info connection_changed";
    case Event::shutdown_failed: return "error shutdown_failed";
    case Event::request_failed: return "error request_failed";
    case Event::command_failed: return "error command_failed";
    }
    return "error unknown_event";
}

}

Logger::Logger(std::ostream& output) : output_(output) {}

void Logger::write(Event event) {
    const std::lock_guard<std::mutex> lock(mutex_);
    output_ << event_name(event) << '\n';
    output_.flush();
}

void Logger::request_completed(std::chrono::milliseconds duration, std::size_t pending) {
    const std::lock_guard<std::mutex> lock(mutex_);
    output_ << "info request_completed duration_ms=" << duration.count()
            << " pending=" << pending << '\n';
    output_.flush();
}

}
