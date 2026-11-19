#pragma once

#include "telegram_mcpp/telegram_operations.hpp"

namespace telegram_mcpp {

void serve_mcp(TelegramOperations& operations, Logger& logger, int concurrency,
               const Cancelled& interrupted, int input = 0, int output = 1);

}
