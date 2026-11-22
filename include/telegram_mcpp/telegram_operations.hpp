#pragma once

#include "telegram_mcpp/account.hpp"
#include "telegram_mcpp/cursor.hpp"

#include <atomic>
#include <mutex>

namespace telegram_mcpp {

enum class OperationErrorCode { invalid_input, invalid_cursor, unauthenticated, denied_chat,
                           message_unavailable, history_unavailable, draft_unavailable, rate_limited, timeout,
                           cancelled, overloaded, unavailable };
const char* operation_error_name(OperationErrorCode code);

struct OperationFailure {
    OperationErrorCode code;
    int retry_after = 0;
};

class OperationError : public std::runtime_error {
public:
    explicit OperationError(OperationErrorCode code, int retry_after = 0);
    OperationErrorCode code() const { return failure_.code; }
    int retry_after() const { return failure_.retry_after; }
private:
    OperationFailure failure_;
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
struct DraftResult {
    std::string chat_id;
    bool cleared = false;
};
enum class SearchStatus { complete, partial, failed };
struct MessagePage {
    std::vector<Message> messages;
    std::optional<std::string> next_cursor;
    SearchStatus status = SearchStatus::complete;
    std::optional<OperationFailure> error;
    bool response_limited = false;
};

std::string to_json(const Message& message);
std::string to_json(const AllowedChatPage& page);
std::string to_json(const MessagePage& page);
std::string to_json(const DraftResult& draft);

class TelegramOperations {
public:
    TelegramOperations(AccountDispatcher& dispatcher, const Config& config, CursorCodec::Clock clock = {});
    AllowedChatPage list_chats(const ListChatsInput& input, const Cancelled& cancelled = {});
    MessagePage search_messages(const SearchMessagesInput& input, const Cancelled& cancelled = {});
    Message get_message(const std::string& chat_id, const std::string& message_id,
                        const Cancelled& cancelled = {});
    DraftResult set_draft(const std::string& chat_id, const std::string& text,
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
