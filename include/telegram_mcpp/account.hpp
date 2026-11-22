#pragma once

#include "telegram_mcpp/config.hpp"
#include "telegram_mcpp/logger.hpp"
#include "telegram_mcpp/read_model.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace telegram_mcpp {

enum class AccountErrorCode { invalid_input, unauthenticated, revoked, rate_limited, timeout,
                              cancelled, overloaded, unavailable, rejected, qr_unavailable, unsupported,
                              message_unavailable, history_unavailable, draft_unavailable };

class AccountError : public std::runtime_error {
public:
    explicit AccountError(AccountErrorCode code, int retry_after = 0);
    AccountErrorCode code() const { return code_; }
    int retry_after() const { return retry_after_; }
private:
    AccountErrorCode code_;
    int retry_after_;
};

enum class Authorization { unknown, parameters, phone, qr, email, email_code, code, password,
                           registration, ready, logging_out, closing, closed, unsupported };
struct AuthorizationUpdate {
    Authorization state = Authorization::unknown;
    std::string qr_link;
    std::uint64_t revision = 0;
    bool revoked = false;
};

enum class AccountCommand { parameters, qr, phone, email, email_code, code, password,
                            logout, close, load_chats, chat_ids, chat, search_messages, message, message_link, set_draft };
struct AccountRequest {
    AccountCommand command;
    std::string secret;
    std::int64_t chat_id = 0;
    int limit = 20;
    bool archive = false;
    std::int64_t message_id = 0;
    std::optional<Sender> sender = {};
};
struct ChatSummary {
    std::int64_t id = 0;
    std::string title;
    std::string type;
    bool title_truncated = false;
    std::optional<std::int64_t> peer_user_id = {};
};
struct AccountResult {
    std::optional<AccountErrorCode> error;
    int retry_after = 0;
    bool exhausted = false;
    std::vector<std::int64_t> chat_ids;
    std::optional<ChatSummary> chat;
    std::vector<MessageRecord> messages;
    std::optional<MessageRecord> message;
    std::int64_t next_from_message_id = 0;
    std::optional<std::string> link;
};
struct AccountEvent {
    std::uint64_t request_id = 0;
    AccountResult result;
    std::optional<AuthorizationUpdate> authorization;
};

class AccountTransport {
public:
    virtual ~AccountTransport() = default;
    virtual void send(std::uint64_t id, AccountRequest request) = 0;
    virtual std::optional<AccountEvent> receive(std::chrono::milliseconds timeout) = 0;
};

using Cancelled = std::function<bool()>;

class AccountDispatcher {
public:
    AccountDispatcher(std::unique_ptr<AccountTransport> transport, Logger& logger,
                      int capacity = 4, bool previously_authorized = false);
    ~AccountDispatcher();
    AccountDispatcher(const AccountDispatcher&) = delete;
    AccountDispatcher& operator=(const AccountDispatcher&) = delete;
    AccountResult call(AccountRequest request, std::chrono::milliseconds timeout,
                       const Cancelled& cancelled);
    AuthorizationUpdate authorization() const;
    AuthorizationUpdate wait_authorization(std::uint64_t revision,
                                           std::chrono::milliseconds timeout,
                                           const Cancelled& cancelled);
    void shutdown();
private:
    struct Pending {
        std::uint64_t id;
        AccountRequest request;
        std::promise<AccountResult> promise;
        bool sent = false;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    };
    void run();
    void dispatch();
    void accept(AccountEvent event);
    void fail_pending(AccountErrorCode error);
    std::unique_ptr<AccountTransport> transport_;
    Logger& logger_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Pending>> queue_;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    AuthorizationUpdate authorization_;
    std::uint64_t next_id_ = 1;
    bool stopping_ = false;
    bool failed_ = false;
    bool closing_ = false;
    bool was_ready_ = false;
    bool logout_requested_ = false;
    std::thread worker_;
};

class AccountPrompt {
public:
    virtual ~AccountPrompt() = default;
    virtual std::string secret(const std::string& prompt) = 0;
    virtual void qr(const std::string& link) = 0;
    virtual void rejected() = 0;
};

struct ChatPage {
    std::vector<ChatSummary> chats;
    bool exhausted = false;
    bool partial = false;
};

class AccountAccess {
public:
    AccountAccess(AccountDispatcher& dispatcher, std::chrono::milliseconds timeout,
                  Cancelled cancelled);
    AuthorizationUpdate status();
    void login(AccountPrompt& prompt, bool phone);
    void logout();
    ChatPage chats(bool archive, int offset, int limit);
    void close();
private:
    AccountResult call(AccountRequest request);
    AuthorizationUpdate initialize();
    AccountDispatcher& dispatcher_;
    std::chrono::milliseconds timeout_;
    Cancelled cancelled_;
};

std::string terminal_text(const std::string& text);

}
