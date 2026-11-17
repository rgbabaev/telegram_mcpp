#include "telegram_mcpp/account.hpp"

#include <algorithm>
#include <utility>

namespace telegram_mcpp {
namespace {

const char* error_message(AccountErrorCode code) {
    switch (code) {
    case AccountErrorCode::invalid_input: return "Invalid account command input";
    case AccountErrorCode::unauthenticated: return "Session is not authorized; run auth login";
    case AccountErrorCode::revoked: return "Session authorization was revoked; run auth login";
    case AccountErrorCode::rate_limited: return "Telegram rate limit; retry after the reported delay";
    case AccountErrorCode::timeout: return "Telegram request timed out";
    case AccountErrorCode::cancelled: return "Operation cancelled";
    case AccountErrorCode::overloaded: return "Telegram request capacity exceeded";
    case AccountErrorCode::unavailable: return "Telegram service unavailable";
    case AccountErrorCode::rejected: return "Telegram rejected the request";
    case AccountErrorCode::qr_unavailable: return "QR login was rejected; rerun auth login with --phone";
    case AccountErrorCode::unsupported: return "This authorization step requires an official Telegram app";
    }
    return "Account operation failed";
}

bool needs_login(Authorization state) {
    return state == Authorization::phone || state == Authorization::qr ||
           state == Authorization::code || state == Authorization::password ||
           state == Authorization::email || state == Authorization::email_code ||
           state == Authorization::registration;
}

}

AccountError::AccountError(AccountErrorCode code, int retry_after)
    : std::runtime_error(error_message(code)), code_(code), retry_after_(retry_after) {}

AccountDispatcher::AccountDispatcher(std::unique_ptr<AccountTransport> transport,
                                     Logger& logger, int capacity, bool previously_authorized)
    : transport_(std::move(transport)), logger_(logger), capacity_(capacity) {
    if (!transport_ || capacity < 1 || capacity > 4) {
        throw AccountError(AccountErrorCode::invalid_input);
    }
    was_ready_ = previously_authorized;
    worker_ = std::thread([this] { run(); });
}

AccountDispatcher::~AccountDispatcher() { shutdown(); }

AccountResult AccountDispatcher::call(AccountRequest request, std::chrono::milliseconds timeout,
                                      const Cancelled& cancelled) {
    if (cancelled && cancelled()) throw AccountError(AccountErrorCode::cancelled);
    if (timeout.count() <= 0) throw AccountError(AccountErrorCode::invalid_input);
    auto pending = std::make_shared<Pending>();
    pending->request = std::move(request);
    auto future = pending->promise.get_future();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || failed_) throw AccountError(AccountErrorCode::unavailable);
        if (closing_) throw AccountError(AccountErrorCode::unavailable);
        if (pending->request.command != AccountCommand::close && pending_.size() >= capacity_) {
            throw AccountError(AccountErrorCode::overloaded);
        }
        if (pending->request.command == AccountCommand::close) closing_ = true;
        pending->id = next_id_++;
        pending_.emplace(pending->id, pending);
        queue_.push_back(pending);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const bool cancel = cancelled && cancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!pending->sent) pending_.erase(pending->id);
            queue_.erase(std::remove(queue_.begin(), queue_.end(), pending), queue_.end());
            throw AccountError(cancel ? AccountErrorCode::cancelled : AccountErrorCode::timeout);
        }
        if (future.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) {
            auto result = future.get();
            if (result.error) throw AccountError(*result.error, result.retry_after);
            return result;
        }
    }
}

AuthorizationUpdate AccountDispatcher::authorization() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return authorization_;
}

AuthorizationUpdate AccountDispatcher::wait_authorization(std::uint64_t revision,
    std::chrono::milliseconds timeout, const Cancelled& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (authorization_.revision == revision) {
        if (cancelled && cancelled()) throw AccountError(AccountErrorCode::cancelled);
        if (failed_ || stopping_) throw AccountError(AccountErrorCode::unavailable);
        if (std::chrono::steady_clock::now() >= deadline) throw AccountError(AccountErrorCode::timeout);
        changed_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return authorization_;
}

void AccountDispatcher::dispatch() {
    std::shared_ptr<Pending> pending;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return;
        pending = queue_.front();
        queue_.pop_front();
        if (pending_.count(pending->id) == 0) return;
        pending->sent = true;
        if (pending->request.command == AccountCommand::logout) logout_requested_ = true;
    }
    transport_->send(pending->id, std::move(pending->request));
}

void AccountDispatcher::accept(AccountEvent event) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (event.authorization) {
        const auto state = event.authorization->state;
        const bool revoked = authorization_.revoked ||
            (was_ready_ && !logout_requested_ && (needs_login(state) || state == Authorization::logging_out));
        was_ready_ = was_ready_ || state == Authorization::ready;
        event.authorization->revision = authorization_.revision + 1;
        event.authorization->revoked = state != Authorization::ready && revoked;
        authorization_ = std::move(*event.authorization);
        logger_.write(Event::authorization_changed);
        changed_.notify_all();
    }
    if (event.request_id == 0) return;
    const auto entry = pending_.find(event.request_id);
    if (entry == pending_.end()) return;
    if (entry->second->request.command == AccountCommand::logout && event.result.error) logout_requested_ = false;
    logger_.request_completed(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - entry->second->started), pending_.size() - 1);
    entry->second->promise.set_value(std::move(event.result));
    pending_.erase(entry);
}

void AccountDispatcher::fail_pending(AccountErrorCode error) {
    for (auto& entry : pending_) {
        AccountResult result;
        result.error = error;
        entry.second->promise.set_value(std::move(result));
    }
    pending_.clear();
    queue_.clear();
    changed_.notify_all();
}

void AccountDispatcher::run() {
    try {
        for (;;) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) break;
            }
            dispatch();
            if (auto event = transport_->receive(std::chrono::milliseconds(20))) accept(std::move(*event));
        }
    } catch (...) {
        const std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        logger_.write(Event::telegram_unavailable);
        fail_pending(AccountErrorCode::unavailable);
    }
}

void AccountDispatcher::shutdown() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        fail_pending(AccountErrorCode::cancelled);
    }
    if (worker_.joinable()) worker_.join();
}

AccountAccess::AccountAccess(AccountDispatcher& dispatcher, std::chrono::milliseconds timeout,
                             Cancelled cancelled)
    : dispatcher_(dispatcher), timeout_(timeout), cancelled_(std::move(cancelled)) {}

AccountResult AccountAccess::call(AccountRequest request) {
    return dispatcher_.call(std::move(request), timeout_, cancelled_);
}

AuthorizationUpdate AccountAccess::initialize() {
    auto update = dispatcher_.authorization();
    if (update.state == Authorization::unknown) {
        update = dispatcher_.wait_authorization(update.revision, timeout_, cancelled_);
    }
    if (update.state == Authorization::parameters) {
        call({AccountCommand::parameters, {}});
        update = dispatcher_.authorization();
        while (update.state == Authorization::parameters) {
            update = dispatcher_.wait_authorization(update.revision, timeout_, cancelled_);
        }
    }
    return update;
}

AuthorizationUpdate AccountAccess::status() { return initialize(); }

void AccountAccess::login(AccountPrompt& prompt, bool phone) {
    auto update = initialize();
    for (;;) {
        if (cancelled_ && cancelled_()) throw AccountError(AccountErrorCode::cancelled);
        if (update.state == Authorization::ready) return;
        try {
            switch (update.state) {
            case Authorization::phone:
                if (phone) call({AccountCommand::phone, prompt.secret("Phone number (international format): ")});
                else call({AccountCommand::qr, {}});
                break;
            case Authorization::qr: prompt.qr(update.qr_link); break;
            case Authorization::email: call({AccountCommand::email, prompt.secret("Login email: ")}); break;
            case Authorization::email_code: call({AccountCommand::email_code, prompt.secret("Email code: ")}); break;
            case Authorization::code: call({AccountCommand::code, prompt.secret("Verification code: ")}); break;
            case Authorization::password: call({AccountCommand::password, prompt.secret("2FA password: ")}); break;
            case Authorization::registration:
            case Authorization::unsupported: throw AccountError(AccountErrorCode::unsupported);
            default: throw AccountError(AccountErrorCode::unauthenticated);
            }
        } catch (const AccountError& error) {
            if (error.code() != AccountErrorCode::rejected) throw;
            if (update.state == Authorization::phone && !phone) throw AccountError(AccountErrorCode::qr_unavailable);
            prompt.rejected();
            update = dispatcher_.authorization();
            continue;
        }
        for (;;) {
            try {
                update = dispatcher_.wait_authorization(update.revision, timeout_, cancelled_);
                break;
            } catch (const AccountError& error) {
                if (error.code() != AccountErrorCode::timeout || update.state != Authorization::qr) throw;
            }
        }
    }
}

void AccountAccess::logout() {
    const auto update = initialize();
    if (update.state != Authorization::ready) {
        throw AccountError(update.revoked ? AccountErrorCode::revoked : AccountErrorCode::unauthenticated);
    }
    call({AccountCommand::logout, {}});
    auto state = dispatcher_.authorization();
    while (state.state != Authorization::closed) {
        state = dispatcher_.wait_authorization(state.revision, timeout_, cancelled_);
    }
}

ChatPage AccountAccess::chats(bool archive, int offset, int limit) {
    if (offset < 0 || offset > 9900 || limit < 1 || limit > 100) {
        throw AccountError(AccountErrorCode::invalid_input);
    }
    const auto state = initialize();
    if (state.state != Authorization::ready) {
        throw AccountError(state.revoked ? AccountErrorCode::revoked : AccountErrorCode::unauthenticated);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    const auto bounded_call = [&](AccountRequest request) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) throw AccountError(AccountErrorCode::timeout);
        return dispatcher_.call(std::move(request), left, cancelled_);
    };
    std::vector<std::int64_t> ids;
    ChatPage page;
    std::size_t previous_count = 0;
    bool loaded = false;
    for (int attempt = 0; attempt <= 100; ++attempt) {
        AccountRequest request{AccountCommand::chat_ids, {}};
        request.archive = archive;
        request.limit = offset + limit;
        ids = bounded_call(request).chat_ids;
        if (ids.size() > static_cast<std::size_t>(offset + limit)) ids.resize(offset + limit);
        if (static_cast<int>(ids.size()) >= offset + limit || page.exhausted) break;
        if ((loaded && ids.size() <= previous_count) || attempt == 100 || std::chrono::steady_clock::now() >= deadline) {
            page.partial = true;
            break;
        }
        request.command = AccountCommand::load_chats;
        request.limit = 100;
        previous_count = ids.size();
        loaded = true;
        page.exhausted = bounded_call(request).exhausted;
    }
    for (std::size_t index = static_cast<std::size_t>(offset); index < ids.size(); ++index) {
        AccountRequest request{AccountCommand::chat, {}};
        request.chat_id = ids[index];
        auto result = bounded_call(request);
        if (!result.chat) throw AccountError(AccountErrorCode::unavailable);
        page.chats.push_back(std::move(*result.chat));
    }
    return page;
}

void AccountAccess::close() {
    auto state = dispatcher_.authorization();
    if (state.state == Authorization::closed) return;
    dispatcher_.call({AccountCommand::close, {}}, std::chrono::seconds(5), {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((state = dispatcher_.authorization()).state != Authorization::closed) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) throw AccountError(AccountErrorCode::timeout);
        dispatcher_.wait_authorization(state.revision, left, {});
    }
}

std::string terminal_text(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char character : text) {
        if (character == '\\' || character == '"') result += '\\';
        if (character >= 32 && character < 127) result += static_cast<char>(character);
        else {
            result += "\\x";
            result += hex[character >> 4];
            result += hex[character & 15];
        }
    }
    return result + '"';
}

}
