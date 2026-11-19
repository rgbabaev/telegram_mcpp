#pragma once

#include "telegram_mcpp/account.hpp"
#include "telegram_mcpp/cursor.hpp"

#include <atomic>
#include <mutex>

namespace telegram_mcpp {

enum class ReadErrorCode { invalid_input, invalid_cursor, unauthenticated, denied_chat,
                           message_unavailable, history_unavailable, rate_limited, timeout,
                           cancelled, overloaded, unavailable };
const char* read_error_name(ReadErrorCode code);

struct ReadFailure {
    ReadErrorCode code;
    int retry_after = 0;
};

class ReadError : public std::runtime_error {
public:
    explicit ReadError(ReadErrorCode code, int retry_after = 0);
    ReadErrorCode code() const { return failure_.code; }
    int retry_after() const { return failure_.retry_after; }
private:
    ReadFailure failure_;
};

struct ListChatsInput {
    std::optional<int> limit;
    std::optional<std::string> cursor;
};
struct SenderInput {
    std::string type;
    std::string id;
};
struct SearchMessagesInput {
    std::optional<std::string> query;
    std::optional<std::string> chat_id;
    std::optional<std::string> since;
    std::optional<std::string> until;
    std::optional<int> limit;
    std::optional<std::string> cursor;
    std::optional<SenderInput> sender = {};
};
struct Message {
    std::string chat_id;
    std::string chat_title;
    bool chat_title_truncated = false;
    std::string message_id;
    std::string timestamp;
    std::string text;
    std::string content_type;
    bool text_truncated = false;
    std::optional<std::string> link;
    std::optional<Sender> sender = {};
    bool is_outgoing = false;
    std::optional<MessageReactions> reactions = {};
};
struct AllowedChatPage {
    std::vector<ChatSummary> chats;
    std::optional<std::string> next_cursor;
};
enum class SearchStatus { complete, partial, failed };
struct MessagePage {
    std::vector<Message> messages;
    std::optional<std::string> next_cursor;
    SearchStatus status = SearchStatus::complete;
    std::optional<ReadFailure> error;
    bool response_limited = false;
};

std::string to_json(const Message& message);
std::string to_json(const AllowedChatPage& page);
std::string to_json(const MessagePage& page);

class ReadOperations {
public:
    ReadOperations(AccountDispatcher& dispatcher, const Config& config, CursorCodec::Clock clock = {});
    AllowedChatPage list_chats(const ListChatsInput& input, const Cancelled& cancelled = {});
    MessagePage search_messages(const SearchMessagesInput& input, const Cancelled& cancelled = {});
    Message get_message(const std::string& chat_id, const std::string& message_id,
                        const Cancelled& cancelled = {});
private:
    class Operation;
    AccountResult call(AccountRequest request, Operation& operation);
    Message present(MessageRecord record, const ChatSummary& chat, Operation& operation);
    ChatSummary chat(std::int64_t id, Operation& operation);
    std::optional<std::vector<std::int64_t>> accessible_chats(Operation& operation);
    int page_size(std::optional<int> limit) const;
    void require_allowed(std::int64_t id) const;
    std::string binding(const std::string& operation, int limit, const std::string& filters = {}) const;
    AccountDispatcher& dispatcher_;
    const Config config_;
    const std::vector<std::int64_t> allowed_;
    CursorCodec cursors_;
    std::atomic<int> active_{0};
    std::mutex retry_mutex_;
    std::chrono::steady_clock::time_point retry_until_{};
};

}
