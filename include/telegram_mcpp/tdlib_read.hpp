#pragma once

#include "telegram_mcpp/account.hpp"

#include <td/telegram/td_api.h>

namespace telegram_mcpp {

AccountResult decode_tdlib_error(const td::td_api::error& error, AccountCommand command);
MessageRecord decode_tdlib_message(const td::td_api::message& message);
AccountResult decode_tdlib_read_result(const td::td_api::Object& object, AccountCommand command);
td::td_api::object_ptr<td::td_api::Function> make_tdlib_read_function(const AccountRequest& request);

}
