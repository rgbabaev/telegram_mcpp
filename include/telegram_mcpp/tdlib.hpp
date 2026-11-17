#pragma once

#include "telegram_mcpp/account.hpp"

#include <memory>
#include <string>

namespace telegram_mcpp {

std::string tdlib_version();
std::unique_ptr<AccountTransport> make_tdlib_transport(const Config& config, Logger& logger);

}
