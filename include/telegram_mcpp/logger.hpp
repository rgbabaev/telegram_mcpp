#pragma once

#include <iosfwd>
#include <mutex>

namespace telegram_mcpp {

enum class Event { config_created, config_validated, command_failed };

class Logger {
public:
    explicit Logger(std::ostream& output);
    void write(Event event);

private:
    std::ostream& output_;
    std::mutex mutex_;
};

}
