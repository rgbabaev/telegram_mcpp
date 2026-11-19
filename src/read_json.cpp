#include "telegram_mcpp/read_operations.hpp"
#include "telegram_mcpp/text.hpp"

namespace telegram_mcpp {
namespace {

std::string boolean(bool value) { return value ? "true" : "false"; }
std::string cursor_json(const std::optional<std::string>& cursor) {
    return cursor ? json_string(*cursor) : "null";
}

std::string sender_json(const std::optional<Sender>& sender) {
    if (!sender) return "null";
    return "{\"type\":" + json_string(sender->type == SenderType::user ? "user" : "chat") +
        ",\"id\":" + json_string(std::to_string(sender->id)) + "}";
}

}

std::string to_json(const Message& message) {
    return "{\"chat_id\":" + json_string(message.chat_id) +
        ",\"chat_title\":" + json_string(message.chat_title) +
        ",\"chat_title_truncated\":" + boolean(message.chat_title_truncated) +
        ",\"message_id\":" + json_string(message.message_id) +
        ",\"sender\":" + sender_json(message.sender) +
        ",\"is_outgoing\":" + boolean(message.is_outgoing) +
        ",\"timestamp\":" + json_string(message.timestamp) +
        ",\"text\":" + json_string(message.text) +
        ",\"content_type\":" + json_string(message.content_type) +
        ",\"text_truncated\":" + boolean(message.text_truncated) +
        (message.link ? ",\"link\":" + json_string(*message.link) : "") + "}";
}

std::string to_json(const AllowedChatPage& page) {
    std::string result = "{\"chats\":[";
    for (const auto& chat : page.chats) {
        if (result.back() != '[') result += ',';
        result += "{\"id\":" + json_string(std::to_string(chat.id)) +
            ",\"title\":" + json_string(chat.title) +
            ",\"type\":" + json_string(chat.type) +
            ",\"title_truncated\":" + boolean(chat.title_truncated) +
            (chat.peer_user_id ? ",\"peer_user_id\":" + json_string(std::to_string(*chat.peer_user_id)) : "") + "}";
    }
    return result + "],\"next_cursor\":" + cursor_json(page.next_cursor) + "}";
}

std::string to_json(const MessagePage& page) {
    std::string result = "{\"messages\":[";
    for (const auto& message : page.messages) {
        if (result.back() != '[') result += ',';
        result += to_json(message);
    }
    const char* status = page.status == SearchStatus::complete ? "complete" :
        page.status == SearchStatus::partial ? "partial" : "failed";
    result += "],\"next_cursor\":" + cursor_json(page.next_cursor) +
        ",\"status\":" + json_string(status) + ",\"response_limited\":" + boolean(page.response_limited);
    if (page.error) {
        result += ",\"error\":{\"code\":" + json_string(read_error_name(page.error->code));
        if (page.error->retry_after > 0) result += ",\"retry_after_seconds\":" + std::to_string(page.error->retry_after);
        result += '}';
    }
    return result + '}';
}

}
