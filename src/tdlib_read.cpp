#include "telegram_mcpp/tdlib_read.hpp"
#include "telegram_mcpp/text.hpp"

#include <charconv>
#include <string_view>

namespace telegram_mcpp {
namespace {

namespace api = td::td_api;

AccountErrorCode classify_error(const api::error& error, AccountCommand command) {
    if (error.code_ == 429) return AccountErrorCode::rate_limited;
    if (error.code_ == 401) return AccountErrorCode::unauthenticated;
    if (error.code_ == 400 || error.code_ == 403 || error.code_ == 404) {
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
    if (message.content_) {
        const auto decoded = content(*message.content_);
        result.content_type = decoded.type;
        if (decoded.text) result.text = bounded_utf8(decoded.text->text_, Limits::message_text_bytes, result.text_truncated);
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
