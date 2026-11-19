#pragma once

#include "telegram_mcpp/read_operations.hpp"

namespace telegram_mcpp {

void serve_mcp(ReadOperations& reads, Logger& logger, int concurrency,
               const Cancelled& interrupted, int input = 0, int output = 1);

}
