#include "telegram_mcpp/tdlib_read.hpp"
#include "telegram_mcpp/tdlib_draft.hpp"
#include "telegram_mcpp/text.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace telegram_mcpp;
namespace api = td::td_api;

void require(bool condition) {
    if (!condition) throw std::runtime_error("TDLib mapping expectation failed");
}

void request_mapping() {
    AccountRequest request{AccountCommand::search_messages, "private-query"};
    request.chat_id = INT64_MIN;
    request.message_id = INT64_MAX;
    request.limit = 100;
    const auto function = make_tdlib_read_function(request);
    require(function->get_id() == api::searchChatMessages::ID);
    const auto& search = static_cast<const api::searchChatMessages&>(*function);
    require(search.chat_id_ == INT64_MIN && search.from_message_id_ == INT64_MAX && search.query_ == "private-query");
    require(search.limit_ == 100 && search.offset_ == 0 && !search.sender_id_ && !search.filter_ && !search.topic_id_);
    request.secret.clear();
    for (const auto type : {SenderType::user, SenderType::chat}) {
        request.sender = Sender{type, type == SenderType::user ? INT64_MAX : INT64_MIN};
        const auto filtered = make_tdlib_read_function(request);
        const auto& fields = static_cast<const api::searchChatMessages&>(*filtered);
        require(fields.query_.empty() && fields.from_message_id_ == INT64_MAX && fields.offset_ == 0);
        require(fields.sender_id_ != nullptr);
        if (type == SenderType::user) {
            require(fields.sender_id_->get_id() == api::messageSenderUser::ID);
            require(static_cast<const api::messageSenderUser&>(*fields.sender_id_).user_id_ == INT64_MAX);
        } else {
            require(fields.sender_id_->get_id() == api::messageSenderChat::ID);
            require(static_cast<const api::messageSenderChat&>(*fields.sender_id_).chat_id_ == INT64_MIN);
        }
    }
    request.command = AccountCommand::message;
    const auto message = make_tdlib_read_function(request);
    require(message->get_id() == api::getMessage::ID);
    require(static_cast<const api::getMessage&>(*message).message_id_ == INT64_MAX);
    request.command = AccountCommand::message_link;
    const auto link = make_tdlib_read_function(request);
    require(link->get_id() == api::getMessageLink::ID);
    const auto& fields = static_cast<const api::getMessageLink&>(*link);
    require(fields.chat_id_ == INT64_MIN && fields.message_id_ == INT64_MAX && fields.media_timestamp_ == 0 &&
            fields.checklist_task_id_ == 0 && fields.poll_option_id_.empty() && !fields.for_album_ && !fields.in_message_thread_);
}

void draft_mapping() {
    AccountRequest request{AccountCommand::set_draft, "private-draft: Привіт 👋\nДруга лінія"};
    request.chat_id = INT64_MIN;
    const auto function = make_tdlib_draft_function(request);
    require(function->get_id() == api::setChatDraftMessage::ID);
    const auto& draft = static_cast<const api::setChatDraftMessage&>(*function);
    require(draft.chat_id_ == INT64_MIN && !draft.topic_id_ && draft.draft_message_);
    require(!draft.draft_message_->reply_to_ && !draft.draft_message_->suggested_post_info_ && draft.draft_message_->effect_id_ == 0);
    require(draft.draft_message_->content_ && draft.draft_message_->content_->get_id() == api::draftMessageContentText::ID);
    const auto& content = static_cast<const api::draftMessageContentText&>(*draft.draft_message_->content_);
    require(content.text_ && content.text_->text_ == request.secret && content.text_->entities_.empty() && !content.link_preview_options_);
    request.secret.clear();
    const auto cleared = make_tdlib_draft_function(request);
    const auto& fields = static_cast<const api::setChatDraftMessage&>(*cleared);
    require(fields.chat_id_ == INT64_MIN && !fields.topic_id_ && !fields.draft_message_);
    require(!decode_tdlib_draft_result(api::ok()).error);
    require(decode_tdlib_draft_result(api::chat()).error == AccountErrorCode::unavailable);
    require(decode_tdlib_draft_result(api::message()).error == AccountErrorCode::unavailable);
    request.command = AccountCommand::message;
    try { make_tdlib_draft_function(request); require(false); }
    catch (const AccountError& error) { require(error.code() == AccountErrorCode::invalid_input); }
}

void message_mapping() {
    api::message message;
    message.chat_id_ = INT64_MIN;
    message.id_ = INT64_MAX;
    message.date_ = 1704067200;
    message.sender_id_ = api::make_object<api::messageSenderUser>(INT64_MAX);
    message.is_outgoing_ = true;
    message.forward_info_ = api::make_object<api::messageForwardInfo>();
    message.forward_info_->origin_ = api::make_object<api::messageOriginUser>(123);
    auto text = api::make_object<api::messageText>();
    text->text_ = api::make_object<api::formattedText>();
    text->text_->text_ = std::string(8191, 'x') + "€";
    message.content_ = std::move(text);
    auto result = decode_tdlib_message(message);
    require(result.chat_id == INT64_MIN && result.id == INT64_MAX && result.date == 1704067200);
    require(result.sender && result.sender->type == SenderType::user && result.sender->id == INT64_MAX && result.is_outgoing);
    require(result.text.size() == 8191 && result.text_truncated && valid_utf8(result.text) && result.content_type == "text");
    auto photo = api::make_object<api::messagePhoto>();
    photo->caption_ = api::make_object<api::formattedText>();
    photo->caption_->text_ = "private-caption";
    message.content_ = std::move(photo);
    result = decode_tdlib_message(message);
    require(result.content_type == "photo" && result.text == "private-caption");
    message.sender_id_ = api::make_object<api::messageSenderChat>(INT64_MIN);
    message.is_outgoing_ = false;
    result = decode_tdlib_message(message);
    require(result.sender && result.sender->type == SenderType::chat && result.sender->id == INT64_MIN && !result.is_outgoing);
    auto video = api::make_object<api::messageVideo>();
    video->caption_ = api::make_object<api::formattedText>();
    video->caption_->text_ = "private-video-caption";
    message.content_ = std::move(video);
    require(decode_tdlib_message(message).text == "private-video-caption");
    message.content_ = api::make_object<api::messageExpiredPhoto>();
    result = decode_tdlib_message(message);
    require(result.content_type == "unsupported" && result.text.empty());
    message.content_.reset();
    require(decode_tdlib_message(message).content_type == "unsupported");
    message.sender_id_.reset();
    require(!decode_tdlib_message(message).sender);
}

void private_chat_mapping() {
    api::chat chat;
    chat.id_ = 42;
    chat.title_ = "private-title";
    chat.type_ = api::make_object<api::chatTypePrivate>(INT64_MAX);
    const auto result = decode_tdlib_chat(chat);
    require(result.id == 42 && result.title == chat.title_ && result.type == "private");
    require(result.peer_user_id == INT64_MAX && !result.title_truncated);
    chat.type_ = api::make_object<api::chatTypeBasicGroup>(20);
    require(!decode_tdlib_chat(chat).peer_user_id && decode_tdlib_chat(chat).type == "basic_group");
    chat.type_.reset();
    require(!decode_tdlib_chat(chat).peer_user_id && decode_tdlib_chat(chat).type == "unsupported");
}

api::object_ptr<api::messageReaction> reaction(api::object_ptr<api::ReactionType> type, int count, bool chosen = false) {
    auto result = api::make_object<api::messageReaction>();
    result->type_ = std::move(type);
    result->total_count_ = count;
    result->is_chosen_ = chosen;
    return result;
}

void reaction_mapping() {
    api::message message;
    require(!decode_tdlib_message(message).reactions);
    message.interaction_info_ = api::make_object<api::messageInteractionInfo>();
    require(!decode_tdlib_message(message).reactions);
    auto& source = message.interaction_info_->reactions_;
    source = api::make_object<api::messageReactions>();
    auto result = decode_tdlib_message(message);
    require(result.reactions && result.reactions->items.empty() && !result.reactions->truncated);
    source->are_tags_ = true;
    source->reactions_.push_back(reaction(api::make_object<api::reactionTypeEmoji>("❤️"), 2, true));
    source->reactions_.push_back(reaction(api::make_object<api::reactionTypeCustomEmoji>(INT64_MAX), 3));
    source->reactions_.push_back(reaction(api::make_object<api::reactionTypeCustomEmoji>(INT64_MIN), 4));
    source->reactions_.push_back(reaction(api::make_object<api::reactionTypePaid>(), 5));
    source->reactions_.push_back(reaction(nullptr, 6));
    result = decode_tdlib_message(message);
    require(result.reactions && result.reactions->are_tags && !result.reactions->truncated);
    const auto& items = result.reactions->items;
    require(items.size() == 5);
    require(items[0].type == "emoji" && items[0].emoji == "❤️" && items[0].total_count == 2 && items[0].is_chosen);
    require(items[1].type == "custom_emoji" && items[1].custom_emoji_id == INT64_MAX && !items[1].is_chosen);
    require(items[2].custom_emoji_id == INT64_MIN && items[2].total_count == 4);
    require(items[3].type == "paid" && items[3].total_count == 5);
    require(items[4].type == "unsupported" && items[4].total_count == 6);
    source->reactions_.push_back(nullptr);
    require(decode_tdlib_message(message).reactions->truncated);
    source->reactions_.clear();
    for (int index = 0; index < 101; ++index) {
        source->reactions_.push_back(reaction(api::make_object<api::reactionTypeEmoji>(std::string(63, 'x') + "❤️"), 1));
    }
    result = decode_tdlib_message(message);
    require(result.reactions->items.size() == Limits::maximum_reaction_types && result.reactions->truncated);
    require(result.reactions->items[0].emoji.size() == 63 && valid_utf8(result.reactions->items[0].emoji));
}

void error_mapping() {
    for (const int code : {400, 403, 404}) {
        api::error error(code, "private-raw-error");
        require(decode_tdlib_error(error, AccountCommand::message).error == AccountErrorCode::message_unavailable);
        require(decode_tdlib_error(error, AccountCommand::search_messages).error == AccountErrorCode::history_unavailable);
        require(decode_tdlib_error(error, AccountCommand::chat).error == AccountErrorCode::history_unavailable);
        require(decode_tdlib_error(error, AccountCommand::set_draft).error == AccountErrorCode::draft_unavailable);
    }
    for (const auto& item : std::vector<std::pair<int, AccountErrorCode>>{
             {401, AccountErrorCode::unauthenticated}, {408, AccountErrorCode::timeout},
             {429, AccountErrorCode::rate_limited}, {500, AccountErrorCode::unavailable}, {406, AccountErrorCode::unavailable}}) {
        require(decode_tdlib_error(api::error(item.first, "private-raw-error"), AccountCommand::message).error == item.second);
    }
    const auto limited = decode_tdlib_error(api::error(429, "Too Many Requests: retry after 42"), AccountCommand::search_messages);
    require(limited.error == AccountErrorCode::rate_limited && limited.retry_after == 42);
    for (const auto* invalid : {"private-raw-error", "Too Many Requests: retry after -1", "Too Many Requests: retry after 9999999999999"}) {
        require(decode_tdlib_error(api::error(429, invalid), AccountCommand::search_messages).retry_after == 0);
    }
}

void response_mapping() {
    require(decode_tdlib_read_result(api::ok(), AccountCommand::search_messages).error == AccountErrorCode::unavailable);
    api::foundChatMessages found;
    found.next_from_message_id_ = 42;
    auto message = api::make_object<api::message>();
    message->id_ = INT64_MAX;
    message->chat_id_ = INT64_MIN;
    found.messages_.push_back(std::move(message));
    auto result = decode_tdlib_read_result(found, AccountCommand::search_messages);
    require(!result.error && result.messages.size() == 1 && result.next_from_message_id == 42);
    require(result.messages[0].id == INT64_MAX);
    found.messages_.push_back(nullptr);
    require(decode_tdlib_read_result(found, AccountCommand::search_messages).error == AccountErrorCode::history_unavailable);
    require(decode_tdlib_read_result(found, AccountCommand::message).error == AccountErrorCode::unavailable);
    api::message single;
    single.id_ = 123;
    result = decode_tdlib_read_result(single, AccountCommand::message);
    require(result.message && result.message->id == 123);
    api::messageLink link("https://t.me/example/1", true);
    result = decode_tdlib_read_result(link, AccountCommand::message_link);
    require(result.link && *result.link == link.link_);
}

}

int main() {
    try { request_mapping(); draft_mapping(); message_mapping(); private_chat_mapping(); reaction_mapping(); response_mapping(); error_mapping(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
    return EXIT_SUCCESS;
}
