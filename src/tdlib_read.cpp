#include "telegram_mcpp/tdlib_read.hpp"
#include "telegram_mcpp/text.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace telegram_mcpp {
namespace {

namespace api = td::td_api;

AccountErrorCode classify_error(const api::error& error, AccountCommand command) {
    if (error.code_ == 429) return AccountErrorCode::rate_limited;
    if (error.code_ == 401) return AccountErrorCode::unauthenticated;
    if (error.code_ == 400 || error.code_ == 403 || error.code_ == 404) {
        if (command == AccountCommand::set_draft) return AccountErrorCode::draft_unavailable;
        if (command == AccountCommand::message || command == AccountCommand::message_link) return AccountErrorCode::message_unavailable;
        if (command == AccountCommand::search_messages || command == AccountCommand::chat) return AccountErrorCode::history_unavailable;
        if (error.code_ == 400) return AccountErrorCode::rejected;
    }
    if (error.code_ == 408) return AccountErrorCode::timeout;
    return AccountErrorCode::unavailable;
}

int retry_delay(const api::error& error) {
    constexpr std::string_view prefix = "Too Many Requests: retry after ";
    const std::string_view message(error.message_);
    if (error.code_ != 429 || message.substr(0, prefix.size()) != prefix) return 0;
    int seconds = 0;
    const auto value = message.substr(prefix.size());
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds > 0 ? seconds : 0;
}

struct Content {
    std::string type = "unsupported";
    const api::formattedText* text = nullptr;
};

std::optional<Sender> sender(const api::MessageSender* value) {
    if (!value) return {};
    switch (value->get_id()) {
    case api::messageSenderUser::ID:
        return Sender{SenderType::user, static_cast<const api::messageSenderUser&>(*value).user_id_};
    case api::messageSenderChat::ID:
        return Sender{SenderType::chat, static_cast<const api::messageSenderChat&>(*value).chat_id_};
    default: return {};
    }
}

Reaction reaction(const api::messageReaction& value, bool& truncated) {
    Reaction result;
    result.total_count = value.total_count_;
    result.is_chosen = value.is_chosen_;
    if (!value.type_) return result;
    switch (value.type_->get_id()) {
    case api::reactionTypeEmoji::ID:
        result.type = "emoji";
        result.emoji = bounded_utf8(static_cast<const api::reactionTypeEmoji&>(*value.type_).emoji_,
                                    Limits::reaction_emoji_bytes, truncated);
        break;
    case api::reactionTypeCustomEmoji::ID:
        result.type = "custom_emoji";
        result.custom_emoji_id = static_cast<const api::reactionTypeCustomEmoji&>(*value.type_).custom_emoji_id_;
        break;
    case api::reactionTypePaid::ID: result.type = "paid"; break;
    default: break;
    }
    return result;
}

std::optional<MessageReactions> reactions(const api::messageInteractionInfo* interaction) {
    if (!interaction || !interaction->reactions_) return {};
    const auto& source = *interaction->reactions_;
    MessageReactions result;
    result.are_tags = source.are_tags_;
    result.truncated = source.reactions_.size() > Limits::maximum_reaction_types;
    const auto count = std::min(source.reactions_.size(), Limits::maximum_reaction_types);
    for (std::size_t index = 0; index < count; ++index) {
        if (source.reactions_[index]) result.items.push_back(reaction(*source.reactions_[index], result.truncated));
        else result.truncated = true;
    }
    return result;
}

std::string chat_type(const api::ChatType* type) {
    if (!type) return "unsupported";
    switch (type->get_id()) {
    case api::chatTypePrivate::ID: return "private";
    case api::chatTypeBasicGroup::ID: return "basic_group";
    case api::chatTypeSupergroup::ID:
        return static_cast<const api::chatTypeSupergroup&>(*type).is_channel_ ? "channel" : "supergroup";
    case api::chatTypeSecret::ID: return "secret";
    default: return "unsupported";
    }
}

Content content(const api::MessageContent& value) {
    switch (value.get_id()) {
    case api::messageText::ID: return {"text", static_cast<const api::messageText&>(value).text_.get()};
    case api::messagePhoto::ID: return {"photo", static_cast<const api::messagePhoto&>(value).caption_.get()};
    case api::messageVideo::ID: return {"video", static_cast<const api::messageVideo&>(value).caption_.get()};
    case api::messageAnimation::ID: return {"animation", static_cast<const api::messageAnimation&>(value).caption_.get()};
    case api::messageAudio::ID: return {"audio", static_cast<const api::messageAudio&>(value).caption_.get()};
    case api::messageDocument::ID: return {"document", static_cast<const api::messageDocument&>(value).caption_.get()};
    case api::messageVoiceNote::ID: return {"voice_note", static_cast<const api::messageVoiceNote&>(value).caption_.get()};
    case api::messageVideoNote::ID: return {"video_note", nullptr};
    case api::messageSticker::ID: return {"sticker", nullptr};
    case api::messagePoll::ID: {
        const auto& poll = static_cast<const api::messagePoll&>(value).poll_;
        return {"poll", poll ? poll->question_.get() : nullptr};
    }
    case api::messageLocation::ID: return {"location", nullptr};
    case api::messageContact::ID: return {"contact", nullptr};
    default: return {};
    }
}

}

AccountResult decode_tdlib_error(const td::td_api::error& error, AccountCommand command) {
    AccountResult result;
    result.error = classify_error(error, command);
    result.retry_after = retry_delay(error);
    return result;
}

MessageRecord decode_tdlib_message(const td::td_api::message& message) {
    MessageRecord result;
    result.chat_id = message.chat_id_;
    result.id = message.id_;
    result.date = message.date_;
    result.sender = sender(message.sender_id_.get());
    result.is_outgoing = message.is_outgoing_;
    result.reactions = reactions(message.interaction_info_.get());
    if (message.content_) {
        const auto decoded = content(*message.content_);
        result.content_type = decoded.type;
        if (decoded.text) result.text = bounded_utf8(decoded.text->text_, Limits::message_text_bytes, result.text_truncated);
    }
    return result;
}

ChatSummary decode_tdlib_chat(const td::td_api::chat& chat) {
    ChatSummary result;
    result.id = chat.id_;
    result.title = bounded_utf8(chat.title_, 512, result.title_truncated);
    result.type = chat_type(chat.type_.get());
    if (result.type == "private") {
        result.peer_user_id = static_cast<const api::chatTypePrivate&>(*chat.type_).user_id_;
    }
    return result;
}

AccountResult decode_tdlib_read_result(const td::td_api::Object& object, AccountCommand command) {
    AccountResult result;
    switch (object.get_id()) {
    case api::foundChatMessages::ID: {
        const auto& found = static_cast<const api::foundChatMessages&>(object);
        if (command != AccountCommand::search_messages || found.messages_.size() > 100 || found.next_from_message_id_ < 0) {
            result.error = AccountErrorCode::unavailable;
            break;
        }
        result.next_from_message_id = found.next_from_message_id_;
        for (const auto& message : found.messages_) {
            if (!message) { result.error = AccountErrorCode::history_unavailable; break; }
            result.messages.push_back(decode_tdlib_message(*message));
        }
        break;
    }
    case api::message::ID:
        if (command == AccountCommand::message) result.message = decode_tdlib_message(static_cast<const api::message&>(object));
        else result.error = AccountErrorCode::unavailable;
        break;
    case api::messageLink::ID:
        if (command == AccountCommand::message_link) result.link = static_cast<const api::messageLink&>(object).link_;
        else result.error = AccountErrorCode::unavailable;
        break;
    default: result.error = AccountErrorCode::unavailable; break;
    }
    return result;
}

td::td_api::object_ptr<td::td_api::Function> make_tdlib_read_function(const AccountRequest& request) {
    switch (request.command) {
    case AccountCommand::search_messages: {
        auto function = api::make_object<api::searchChatMessages>();
        function->chat_id_ = request.chat_id;
        function->query_ = request.secret;
        function->from_message_id_ = request.message_id;
        function->offset_ = 0;
        function->limit_ = request.limit;
        if (request.sender) {
            if (request.sender->type == SenderType::user) {
                function->sender_id_ = api::make_object<api::messageSenderUser>(request.sender->id);
            } else {
                function->sender_id_ = api::make_object<api::messageSenderChat>(request.sender->id);
            }
        }
        return function;
    }
    case AccountCommand::message: return api::make_object<api::getMessage>(request.chat_id, request.message_id);
    case AccountCommand::message_link: {
        auto function = api::make_object<api::getMessageLink>();
        function->chat_id_ = request.chat_id;
        function->message_id_ = request.message_id;
        return function;
    }
    default: throw AccountError(AccountErrorCode::invalid_input);
    }
}

}
