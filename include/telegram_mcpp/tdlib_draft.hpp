#pragma once

#include "telegram_mcpp/account.hpp"

#include <td/telegram/td_api.h>

namespace telegram_mcpp {

td::td_api::object_ptr<td::td_api::Function> make_tdlib_draft_function(const AccountRequest& request);
AccountResult decode_tdlib_draft_result(const td::td_api::Object& object);

}
