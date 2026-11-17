#include "telegram_mcpp/tdlib.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <stdexcept>

namespace telegram_mcpp {

std::string tdlib_version() {
    auto logging = td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogStream>(
        td::td_api::make_object<td::td_api::logStreamEmpty>()));
    if (logging->get_id() != td::td_api::ok::ID) throw std::runtime_error("Cannot disable TDLib logs");
    auto result = td::ClientManager::execute(td::td_api::make_object<td::td_api::getOption>("version"));
    if (result->get_id() != td::td_api::optionValueString::ID) {
        throw std::runtime_error("Cannot determine TDLib version");
    }
    return static_cast<const td::td_api::optionValueString&>(*result).value_;
}

}
