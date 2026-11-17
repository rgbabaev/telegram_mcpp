#include "telegram_mcpp/account.hpp"
#include "telegram_mcpp/private_storage.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace telegram_mcpp;
using namespace std::chrono_literals;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Expectation failed");
}

template <typename Operation>
void fails(AccountErrorCode code, Operation operation) {
    try { operation(); }
    catch (const AccountError& error) { require(error.code() == code); return; }
    throw std::runtime_error("Expected account error");
}

class FakeTransport final : public AccountTransport {
public:
    using Handler = std::function<void(std::uint64_t, const AccountRequest&, FakeTransport&)>;
    explicit FakeTransport(Handler handler) : handler_(std::move(handler)) {}
    void send(std::uint64_t id, AccountRequest request) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request.command);
            sent_.push_back(id);
            changed_.notify_all();
        }
        handler_(id, request, *this);
    }
    std::optional<AccountEvent> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return !events_.empty(); });
        if (events_.empty()) return {};
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }
    void push(AccountEvent event) {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
        changed_.notify_all();
    }
    void state(Authorization state, std::string qr = {}) {
        AccountEvent event;
        event.authorization = AuthorizationUpdate{state, std::move(qr), 0, false};
        push(std::move(event));
    }
    void reply(std::uint64_t id, AccountResult result = {}) { push({id, std::move(result), {}}); }
    std::vector<std::uint64_t> wait_sent(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, 2s, [&] { return sent_.size() >= count; }));
        return sent_;
    }
    std::vector<AccountCommand> requests() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
private:
    Handler handler_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<AccountEvent> events_;
    std::vector<AccountCommand> requests_;
    std::vector<std::uint64_t> sent_;
};

class Prompt final : public AccountPrompt {
public:
    std::deque<std::string> inputs;
    std::vector<std::string> links;
    int rejections = 0;
    std::string secret(const std::string&) override {
        require(!inputs.empty());
        auto value = inputs.front();
        inputs.pop_front();
        return value;
    }
    void qr(const std::string& link) override { links.push_back(link); }
    void rejected() override { ++rejections; }
};

void qr_and_two_factor() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto& request, auto& fake) {
        fake.reply(id);
        switch (request.command) {
        case AccountCommand::parameters: fake.state(Authorization::phone); break;
        case AccountCommand::qr:
            fake.state(Authorization::qr, "tg://login?token=private-token");
            break;
        case AccountCommand::password:
            require(request.secret == "private-password");
            fake.state(Authorization::ready);
            break;
        case AccountCommand::close: fake.state(Authorization::closed); break;
        default: throw std::runtime_error("Unexpected request");
        }
    });
    auto* fake = transport.get();
    fake->state(Authorization::parameters);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    class QrPrompt final : public AccountPrompt {
    public:
        explicit QrPrompt(FakeTransport& fake) : fake_(fake) {}
        std::string secret(const std::string&) override { return "private-password"; }
        void qr(const std::string& link) override {
            if (link == "tg://login?token=private-token") fake_.state(Authorization::qr, "tg://login?token=private-refresh");
            else { require(link == "tg://login?token=private-refresh"); fake_.state(Authorization::password); }
        }
        void rejected() override { throw std::runtime_error("Unexpected rejection"); }
    private:
        FakeTransport& fake_;
    } prompt(*fake);
    access.login(prompt, false);
    require(access.status().state == Authorization::ready);
    access.close();
    dispatcher.shutdown();
    require(output.str().find("private-") == std::string::npos);
    require(output.str().find("request_completed duration_ms=") != std::string::npos);
}

void phone_fallback_steps_and_rejection() {
    std::ostringstream output;
    Logger logger(output);
    int attempts = 0;
    auto transport = std::make_unique<FakeTransport>([&](auto id, const auto& request, auto& fake) {
        if (request.command == AccountCommand::code && attempts++ == 0) {
            AccountResult result;
            result.error = AccountErrorCode::rejected;
            fake.reply(id, result);
            return;
        }
        fake.reply(id);
        switch (request.command) {
        case AccountCommand::phone: fake.state(Authorization::email); break;
        case AccountCommand::email: fake.state(Authorization::email_code); break;
        case AccountCommand::email_code: fake.state(Authorization::code); break;
        case AccountCommand::code: fake.state(Authorization::password); break;
        case AccountCommand::password: fake.state(Authorization::ready); break;
        default: break;
        }
    });
    transport->state(Authorization::phone);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    Prompt prompt;
    prompt.inputs = {"private-phone", "private-email", "private-email-code", "bad-code", "private-code", "private-password"};
    access.login(prompt, true);
    require(prompt.inputs.empty() && prompt.rejections == 1);
    dispatcher.shutdown();
    require(output.str().find("private-") == std::string::npos);
}

void recovery_and_revocation() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto& request, auto& fake) {
        fake.reply(id);
        if (request.command == AccountCommand::parameters) fake.state(Authorization::ready);
    });
    auto* fake = transport.get();
    fake->state(Authorization::parameters);
    AccountDispatcher dispatcher(std::move(transport), logger, 4, true);
    AccountAccess access(dispatcher, 1s, {});
    require(access.status().state == Authorization::ready);
    Prompt prompt;
    access.login(prompt, false);
    const auto revision = dispatcher.authorization().revision;
    fake->state(Authorization::phone);
    require(dispatcher.wait_authorization(revision, 1s, {}).revoked);
    fails(AccountErrorCode::revoked, [&] { access.chats(false, 0, 20); });
    require(fake->requests() == std::vector<AccountCommand>{AccountCommand::parameters});
}

void revoked_after_restart() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {});
    transport->state(Authorization::phone);
    AccountDispatcher dispatcher(std::move(transport), logger, 4, true);
    AccountAccess access(dispatcher, 1s, {});
    require(access.status().revoked);
}

void status_never_authenticates() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {});
    auto* fake = transport.get();
    fake->state(Authorization::password);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    require(access.status().state == Authorization::password);
    fails(AccountErrorCode::unauthenticated, [&] { access.chats(false, 0, 20); });
    require(fake->requests().empty());
}

void bounded_requests_and_correlation() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {});
    auto* fake = transport.get();
    AccountDispatcher dispatcher(std::move(transport), logger, 2);
    auto request = [&](int limit) {
        AccountRequest command{AccountCommand::chat_ids, {}};
        command.limit = limit;
        return dispatcher.call(command, 2s, {}).chat_ids;
    };
    auto first = std::async(std::launch::async, [&] { return request(1); });
    auto ids = fake->wait_sent(1);
    auto second = std::async(std::launch::async, [&] { return request(2); });
    ids = fake->wait_sent(2);
    fails(AccountErrorCode::overloaded, [&] { request(3); });
    AccountResult response;
    response.chat_ids = {22};
    fake->reply(ids[1], response);
    response.chat_ids = {11};
    fake->reply(ids[0], response);
    require(first.get() == std::vector<std::int64_t>{11});
    require(second.get() == std::vector<std::int64_t>{22});
}

void timeout_cancellation_and_late_reply() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {});
    auto* fake = transport.get();
    AccountDispatcher dispatcher(std::move(transport), logger, 1);
    fails(AccountErrorCode::cancelled, [&] {
        dispatcher.call({AccountCommand::password, "private-password"}, 1s, [] { return true; });
    });
    require(fake->requests().empty());
    fails(AccountErrorCode::timeout, [&] { dispatcher.call({AccountCommand::chat_ids, {}}, 150ms, {}); });
    const auto ids = fake->wait_sent(1);
    fails(AccountErrorCode::overloaded, [&] { dispatcher.call({AccountCommand::chat_ids, {}}, 1s, {}); });
    fake->reply(ids[0]);
    const auto revision = dispatcher.authorization().revision;
    fake->state(Authorization::ready);
    dispatcher.wait_authorization(revision, 1s, {});
    std::atomic<bool> cancelled{false};
    auto future = std::async(std::launch::async, [&] {
        fails(AccountErrorCode::cancelled, [&] {
            dispatcher.call({AccountCommand::chat_ids, {}}, 2s, [&] { return cancelled.load(); });
        });
    });
    fake->wait_sent(2);
    cancelled = true;
    future.get();
    dispatcher.shutdown();
    require(output.str().find("private-password") == std::string::npos);
}

void rate_limit_does_not_logout() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto&, auto& fake) {
        AccountResult result;
        result.error = AccountErrorCode::rate_limited;
        result.retry_after = 42;
        fake.reply(id, result);
    });
    auto* fake = transport.get();
    fake->state(Authorization::phone);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    Prompt prompt;
    try { access.login(prompt, false); throw std::runtime_error("Expected rate limit"); }
    catch (const AccountError& error) { require(error.retry_after() == 42); }
    require(fake->requests() == std::vector<AccountCommand>{AccountCommand::qr});
}

void rejected_qr_does_not_retry() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto&, auto& fake) {
        AccountResult result;
        result.error = AccountErrorCode::rejected;
        fake.reply(id, result);
    });
    auto* fake = transport.get();
    fake->state(Authorization::phone);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    Prompt prompt;
    fails(AccountErrorCode::qr_unavailable, [&] { access.login(prompt, false); });
    require(fake->requests() == std::vector<AccountCommand>{AccountCommand::qr});
}

void disconnect_does_not_logout() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {
        throw std::runtime_error("private-raw-tdlib-error");
    });
    transport->state(Authorization::ready);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    fails(AccountErrorCode::unavailable, [&] { access.chats(false, 0, 20); });
    dispatcher.shutdown();
    require(output.str().find("private-") == std::string::npos);
}

void logout_is_explicit() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto&, auto& fake) {
        fake.reply(id);
        fake.state(Authorization::logging_out);
        fake.state(Authorization::closed);
    });
    auto* fake = transport.get();
    fake->state(Authorization::ready);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    access.logout();
    require(fake->requests() == std::vector<AccountCommand>{AccountCommand::logout});
    require(!dispatcher.authorization().revoked);
}

void chat_loading_and_pagination() {
    std::ostringstream output;
    Logger logger(output);
    int loaded = 0;
    auto transport = std::make_unique<FakeTransport>([&](auto id, const auto& request, auto& fake) {
        if (request.command != AccountCommand::chat) require(request.archive);
        AccountResult result;
        if (request.command == AccountCommand::load_chats) {
            loaded += 2;
            result.exhausted = loaded >= 4;
        } else if (request.command == AccountCommand::chat_ids) {
            for (int index = 0; index < std::min(loaded, request.limit); ++index) result.chat_ids.push_back(INT64_MAX - index);
        } else if (request.command == AccountCommand::chat) {
            result.chat = ChatSummary{request.chat_id, "private-title", "channel", false};
        }
        fake.reply(id, result);
    });
    transport->state(Authorization::ready);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    const auto page = access.chats(true, 1, 3);
    require(!page.partial && page.exhausted && page.chats.size() == 3);
    require(page.chats.front().id == INT64_MAX - 1);
    require(page.chats.back().id == INT64_MAX - 3);
    fails(AccountErrorCode::invalid_input, [&] { access.chats(false, -1, 20); });
    fails(AccountErrorCode::invalid_input, [&] { access.chats(false, 0, 101); });
    dispatcher.shutdown();
    require(output.str().find("private-title") == std::string::npos);
    require(terminal_text("a\n\x1b[31m\"\\") == "\"a\\x0a\\x1b[31m\\\"\\\\\"");
}

void close_with_full_capacity() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto& request, auto& fake) {
        if (request.command == AccountCommand::close) {
            fake.reply(id);
            fake.state(Authorization::closed);
        }
    });
    auto* fake = transport.get();
    AccountDispatcher dispatcher(std::move(transport), logger, 1);
    fails(AccountErrorCode::timeout, [&] { dispatcher.call({AccountCommand::chat_ids, {}}, 150ms, {}); });
    AccountAccess access(dispatcher, 1s, {});
    access.close();
    require(fake->requests() == std::vector<AccountCommand>{AccountCommand::chat_ids, AccountCommand::close});
}

void stalled_discovery_is_partial() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto id, const auto&, auto& fake) { fake.reply(id); });
    transport->state(Authorization::ready);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    const auto page = access.chats(false, 0, 20);
    require(page.partial && !page.exhausted && page.chats.empty());
}

void registration_is_not_automatic() {
    std::ostringstream output;
    Logger logger(output);
    auto transport = std::make_unique<FakeTransport>([](auto, const auto&, auto&) {});
    auto* fake = transport.get();
    fake->state(Authorization::registration);
    AccountDispatcher dispatcher(std::move(transport), logger);
    AccountAccess access(dispatcher, 1s, {});
    Prompt prompt;
    fails(AccountErrorCode::unsupported, [&] { access.login(prompt, true); });
    require(fake->requests().empty());
}

class Workspace {
public:
    Workspace() {
        char pattern[] = "/tmp/telegram-account-tests-XXXXXX";
        const auto path = ::mkdtemp(pattern);
        if (!path) throw std::runtime_error("Cannot create temporary directory");
        directory = path;
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::filesystem::path directory;
};

void locking_and_private_session_files() {
    Workspace workspace;
    std::string key;
    {
        SessionLock lock(workspace.directory);
        key = read_private_file(workspace.directory / ".database.key");
        require(key.size() == 32);
        const auto child = ::fork();
        require(child >= 0);
        if (child == 0) {
            try { SessionLock competing(workspace.directory); ::_exit(1); }
            catch (const std::exception&) { ::_exit(0); }
        }
        int status;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        create_private_file(workspace.directory / "database" / "sample", "private-state");
    }
    {
        SessionLock reopened(workspace.directory);
        require(read_private_file(workspace.directory / ".database.key") == key);
    }
    require(::chmod((workspace.directory / "database" / "sample").c_str(), 0644) == 0);
    const auto previous = ::umask(0022);
    try { SessionLock unsafe(workspace.directory); throw std::logic_error("Unsafe file accepted"); }
    catch (const std::runtime_error&) {}
    const auto restored = ::umask(previous);
    require(restored == 0022);
}

void unsafe_session_entries_are_rejected() {
    Workspace workspace;
    { SessionLock lock(workspace.directory); }
    const auto entry = workspace.directory / "database" / "unsafe";
    std::filesystem::create_symlink(workspace.directory / ".database.key", entry);
    bool rejected = false;
    try { SessionLock lock(workspace.directory); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected);
    std::filesystem::remove(entry);
    require(::mkfifo(entry.c_str(), 0600) == 0);
    rejected = false;
    try { SessionLock lock(workspace.directory); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected);
}

void missing_key_never_resets_database() {
    Workspace workspace;
    {
        SessionLock lock(workspace.directory);
        create_private_file(workspace.directory / "database" / "sample", "private-state");
    }
    std::filesystem::remove(workspace.directory / ".database.key");
    bool failed = false;
    try { SessionLock lock(workspace.directory); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed && !std::filesystem::exists(workspace.directory / ".database.key"));
    require(read_private_file(workspace.directory / "database" / "sample") == "private-state");
}

}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"QR and 2FA", qr_and_two_factor},
        {"phone/email/code/password and retry", phone_fallback_steps_and_rejection},
        {"recovery and revocation", recovery_and_revocation},
        {"persisted revocation", revoked_after_restart},
        {"noninteractive status", status_never_authenticates},
        {"bounded correlation", bounded_requests_and_correlation},
        {"timeout and cancellation", timeout_cancellation_and_late_reply},
        {"rate limit", rate_limit_does_not_logout},
        {"no QR retry loop", rejected_qr_does_not_retry},
        {"disconnect", disconnect_does_not_logout},
        {"explicit logout", logout_is_explicit},
        {"chat loading", chat_loading_and_pagination},
        {"close under overload", close_with_full_capacity},
        {"partial discovery", stalled_discovery_is_partial},
        {"no implicit registration", registration_is_not_automatic},
        {"exclusive ownership", locking_and_private_session_files},
        {"unsafe session entries", unsafe_session_entries_are_rejected},
        {"missing database key", missing_key_never_resets_database}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); }
        catch (const std::exception& error) { std::cerr << name << ": " << error.what() << '\n'; ++failures; }
    }
    std::cerr << tests.size() << " account cases, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
