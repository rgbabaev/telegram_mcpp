#include "telegram_mcpp/tdlib_draft.hpp"

#include <utility>

namespace telegram_mcpp {
namespace api = td::td_api;

api::object_ptr<api::Function> make_tdlib_draft_function(const AccountRequest& request) {
    if (request.command != AccountCommand::set_draft) throw AccountError(AccountErrorCode::invalid_input);
    auto function = api::make_object<api::setChatDraftMessage>();
    function->chat_id_ = request.chat_id;
    if (!request.secret.empty()) {
        auto text = api::make_object<api::formattedText>();
        text->text_ = request.secret;
        auto content = api::make_object<api::draftMessageContentText>();
        content->text_ = std::move(text);
        function->draft_message_ = api::make_object<api::draftMessage>();
        function->draft_message_->content_ = std::move(content);
    }
    return function;
}

AccountResult decode_tdlib_draft_result(const api::Object& object) {
    AccountResult result;
    if (object.get_id() != api::ok::ID) result.error = AccountErrorCode::unavailable;
    return result;
}

}
