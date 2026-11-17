#include "telegram_mcpp/account.hpp"
#include "telegram_mcpp/config.hpp"
#include "telegram_mcpp/logger.hpp"
#include "telegram_mcpp/operator_terminal.hpp"
#include "telegram_mcpp/private_storage.hpp"
#ifdef TELEGRAM_MCPP_WITH_TDLIB
#include "telegram_mcpp/tdlib.hpp"
#endif

#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void interrupt(int signal) { interrupted = signal; }

void install_signals() {
    struct sigaction action {};
    action.sa_handler = interrupt;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, nullptr) || ::sigaction(SIGTERM, &action, nullptr)) {
        throw std::runtime_error("Cannot install shutdown handlers");
    }
}

struct Command {
    std::string_view group;
    std::string_view action;
    std::string path;
    bool phone = false;
    bool archive = false;
    int offset = 0;
    int limit = 0;
};

int integer(std::string_view value) {
    if (value.empty()) throw telegram_mcpp::AccountError(telegram_mcpp::AccountErrorCode::invalid_input);
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw telegram_mcpp::AccountError(telegram_mcpp::AccountErrorCode::invalid_input);
    }
    return result;
}

Command parse(int argc, char* argv[]) {
    using namespace telegram_mcpp;
    if (argc < 4) throw AccountError(AccountErrorCode::invalid_input);
    Command command{argv[1], argv[2], argv[3]};
    const bool config = command.group == "config" && (command.action == "init" || command.action == "check");
    const bool auth = command.group == "auth" && (command.action == "login" || command.action == "status" || command.action == "logout");
    const bool chats = command.group == "chats" && command.action == "list";
    if (!config && !auth && !chats) throw AccountError(AccountErrorCode::invalid_input);
    bool offset = false;
    for (int index = 4; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (auth && command.action == "login" && option == "--phone" && !command.phone) command.phone = true;
        else if (chats && option == "--archive" && !command.archive) command.archive = true;
        else if (chats && option == "--limit" && command.limit == 0 && index + 1 < argc) {
            command.limit = integer(argv[++index]);
            if (command.limit < 1 || command.limit > 100) throw AccountError(AccountErrorCode::invalid_input);
        } else if (chats && option == "--offset" && !offset && index + 1 < argc) {
            command.offset = integer(argv[++index]);
            offset = true;
            if (command.offset < 0 || command.offset > 9900) throw AccountError(AccountErrorCode::invalid_input);
        } else throw AccountError(AccountErrorCode::invalid_input);
    }
    return command;
}

#ifdef TELEGRAM_MCPP_WITH_TDLIB

class CloseSession {
public:
    CloseSession(telegram_mcpp::AccountAccess& access, telegram_mcpp::Logger& logger)
        : access_(access), logger_(logger) {}
    void dismiss() { active_ = false; }
    ~CloseSession() {
        if (!active_) return;
        try { access_.close(); }
        catch (...) { logger_.write(telegram_mcpp::Event::shutdown_failed); }
    }
private:
    bool active_ = true;
    telegram_mcpp::AccountAccess& access_;
    telegram_mcpp::Logger& logger_;
};

int account_operation(const Command& command, const telegram_mcpp::Config& config,
                      telegram_mcpp::AccountAccess& access) {
    using namespace telegram_mcpp;
    const auto marker = config.state_directory / ".authorized";
    if (command.group == "chats") {
        const auto page = access.chats(command.archive, command.offset,
            command.limit ? command.limit : config.limits.page_size);
        for (const auto& chat : page.chats) {
            std::cerr << '"' << chat.id << "\" " << chat.type << ' ' << terminal_text(chat.title);
            if (chat.title_truncated) std::cerr << " [title truncated]";
            std::cerr << '\n';
        }
        std::cerr << (page.partial ? "partial" : page.exhausted ? "end of chat list" : "more chats may be available")
                  << "; next offset " << command.offset + page.chats.size() << '\n';
        return page.partial ? 1 : 0;
    }
    if (command.action == "logout") {
        access.logout();
        std::filesystem::remove(marker);
        std::cerr << "Logged out; TDLib session data was removed\n";
        return 0;
    }
    if (command.action == "login") {
        OperatorTerminal terminal([] { return interrupted != 0; });
        access.login(terminal, command.phone);
    }
    const auto status = access.status();
    if (status.state == Authorization::ready) {
        if (!std::filesystem::exists(marker)) create_private_file(marker, "authorized\n");
        std::cerr << "Authorized saved session (connectivity is not verified)\n";
        return 0;
    }
    if (status.revoked) throw AccountError(AccountErrorCode::revoked);
    throw AccountError(AccountErrorCode::unauthenticated);
}

int account_command(const Command& command, telegram_mcpp::Logger& logger) {
    using namespace telegram_mcpp;
    const auto config = load_config(command.path);
    SessionLock lock(config.state_directory);
    const auto marker = config.state_directory / ".authorized";
    AccountDispatcher dispatcher(make_tdlib_transport(config, logger), logger,
        config.limits.concurrent_requests, std::filesystem::exists(marker));
    const auto cancelled = [] { return interrupted != 0; };
    AccountAccess access(dispatcher, config.limits.request_timeout, cancelled);
    CloseSession close(access, logger);
    const auto result = account_operation(command, config, access);
    access.close();
    close.dismiss();
    return result;
}

#endif

int run(int argc, char* argv[], telegram_mcpp::Logger& logger) {
    using namespace telegram_mcpp;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: telegram-mcpp config <init|check> PATH\n"
                     "       telegram-mcpp auth login PATH [--phone]\n"
                     "       telegram-mcpp auth <status|logout> PATH\n"
                     "       telegram-mcpp chats list PATH [--archive] [--offset N] [--limit N]\n"
                     "       telegram-mcpp <--help|--version>\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cerr << "telegram-mcpp 0.1.0\n";
#ifdef TELEGRAM_MCPP_WITH_TDLIB
        std::cerr << "TDLib " << tdlib_version() << '\n';
#else
        std::cerr << "TDLib disabled (offline build)\n";
#endif
        return 0;
    }
    const auto command = parse(argc, argv);
    if (command.group == "config") {
        if (command.action == "init") {
            initialize_config(command.path);
            logger.write(Event::config_created);
        } else {
            prepare_state_directory(load_config(command.path));
            logger.write(Event::config_validated);
        }
        return 0;
    }
#ifdef TELEGRAM_MCPP_WITH_TDLIB
    return account_command(command, logger);
#else
    throw std::runtime_error("Account commands require a TDLib-enabled build");
#endif
}

}

int main(int argc, char* argv[]) {
    telegram_mcpp::Logger logger(std::cerr);
    try {
        install_signals();
        const auto result = run(argc, argv, logger);
        return interrupted ? 128 + interrupted : result;
    } catch (const telegram_mcpp::AccountError& error) {
        logger.write(telegram_mcpp::Event::command_failed);
        std::cerr << error.what() << '\n';
        if (error.retry_after()) std::cerr << "retry_after_seconds=" << error.retry_after() << '\n';
        if (interrupted) return 128 + interrupted;
        return error.code() == telegram_mcpp::AccountErrorCode::invalid_input ? 2 : 1;
    } catch (const std::filesystem::filesystem_error&) {
        logger.write(telegram_mcpp::Event::command_failed);
        std::cerr << "Private storage operation failed\n";
        return interrupted ? 128 + interrupted : 1;
    } catch (const std::exception& error) {
        logger.write(telegram_mcpp::Event::command_failed);
        std::cerr << error.what() << '\n';
        return interrupted ? 128 + interrupted : 1;
    }
}
