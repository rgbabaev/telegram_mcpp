#include "telegram_mcpp/operator_terminal.hpp"
#include "telegram_mcpp/private_storage.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

void require(bool condition) { if (!condition) throw std::runtime_error("Operator test failed"); }

class Terminal {
public:
    Terminal() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        require(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0);
        slave = ::ptsname(master);
    }
    ~Terminal() { ::close(master); }
    int master;
    std::string slave;
};

bool wait_prompt(int descriptor, const std::string& prompt, std::string& captured) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd poll_descriptor{descriptor, POLLIN, 0};
        if (::poll(&poll_descriptor, 1, 50) < 0 && errno != EINTR) return false;
        if (!(poll_descriptor.revents & POLLIN)) continue;
        char buffer[2048];
        const auto count = ::read(descriptor, buffer, sizeof(buffer));
        if (count <= 0) return false;
        captured.append(buffer, static_cast<std::size_t>(count));
        if (captured.find(prompt) != std::string::npos) return true;
    }
    return false;
}

int wait_child(pid_t child) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) return status;
        if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for operator test");
        ::poll(nullptr, 0, 20);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    throw std::runtime_error("Operator test timed out");
}

void hidden_input(bool cancel) {
    Terminal terminal;
    const auto child = ::fork();
    require(child >= 0);
    if (child == 0) {
        ::close(terminal.master);
        if (::setsid() < 0) ::_exit(2);
        const int slave = ::open(terminal.slave.c_str(), O_RDWR);
        if (slave < 0) ::_exit(3);
        termios previous{};
        if (::tcgetattr(slave, &previous) != 0) ::_exit(4);
        ::signal(SIGTERM, interrupt);
        int result = 0;
        try {
            telegram_mcpp::OperatorTerminal input([] { return interrupted != 0; });
            const auto secret = input.secret("Password: ");
            if (cancel || secret != "private-secret") result = 5;
        } catch (const telegram_mcpp::AccountError& error) {
            if (!cancel || error.code() != telegram_mcpp::AccountErrorCode::cancelled) result = 6;
        } catch (...) { result = 7; }
        termios restored{};
        if (::tcgetattr(slave, &restored) != 0 || restored.c_lflag != previous.c_lflag) result = 8;
        ::_exit(result);
    }
    std::string captured;
    if (!wait_prompt(terminal.master, "Password: ", captured)) {
        ::kill(child, SIGKILL); ::waitpid(child, nullptr, 0);
        throw std::runtime_error("Missing terminal prompt");
    }
    if (cancel) ::kill(child, SIGTERM);
    else require(::write(terminal.master, "private-secret\n", 15) == 15);
    const auto status = wait_child(child);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char buffer[1024];
    pollfd ready{terminal.master, POLLIN, 0};
    while (::poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN)) {
        const auto count = ::read(terminal.master, buffer, sizeof(buffer));
        if (count <= 0) break;
        captured.append(buffer, static_cast<std::size_t>(count));
    }
    require(captured.find("private-secret") == std::string::npos);
}

void qr_rendering() {
    Terminal terminal;
    const auto child = ::fork();
    require(child >= 0);
    if (child == 0) {
        ::close(terminal.master);
        if (::setsid() < 0) ::_exit(2);
        if (::open(terminal.slave.c_str(), O_RDWR) < 0) ::_exit(3);
        try {
            telegram_mcpp::OperatorTerminal input({});
            input.qr("tg://login?token=private-qr-test");
            ::_exit(0);
        } catch (...) { ::_exit(4); }
    }
    std::string captured;
    if (!wait_prompt(terminal.master, "Scan this QR code", captured)) {
        ::kill(child, SIGKILL); ::waitpid(child, nullptr, 0);
        throw std::runtime_error("QR renderer did not run");
    }
    const auto status = wait_child(child);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char buffer[4096];
    pollfd ready{terminal.master, POLLIN, 0};
    while (::poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN)) {
        const auto count = ::read(terminal.master, buffer, sizeof(buffer));
        if (count <= 0) break;
        captured.append(buffer, static_cast<std::size_t>(count));
    }
    require(captured.find("private-qr-test") == std::string::npos);
    require(captured.find("\x1b[") != std::string::npos);
}

std::string read_file(const std::filesystem::path& path) {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    require(descriptor >= 0);
    std::string result;
    char buffer[2048];
    ssize_t count;
    while ((count = ::read(descriptor, buffer, sizeof(buffer))) > 0) result.append(buffer, count);
    ::close(descriptor);
    return result;
}

void lifecycle(const char* program) {
    char pattern[] = "/tmp/telegram-operator-tests-XXXXXX";
    const auto path = ::mkdtemp(pattern);
    require(path != nullptr);
    const std::filesystem::path directory(path);
    try {
        const auto config = directory / "config.local";
        const auto output = directory / "stdout";
        const auto errors = directory / "stderr";
        telegram_mcpp::create_private_file(config,
            "api_id=12345\napi_hash=0123456789abcdef0123456789abcdef\nstate_directory=state\nallowed_chat_ids=\n");
        Terminal terminal;
        const auto child = ::fork();
        require(child >= 0);
        if (child == 0) {
            ::close(terminal.master);
            if (::setsid() < 0) ::_exit(2);
            const int slave = ::open(terminal.slave.c_str(), O_RDWR);
            const int out = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            const int err = ::open(errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (slave < 0 || out < 0 || err < 0) ::_exit(3);
            ::dup2(slave, STDIN_FILENO);
            ::dup2(out, STDOUT_FILENO);
            ::dup2(err, STDERR_FILENO);
            ::execl(program, program, "auth", "login", config.c_str(), "--phone", nullptr);
            ::_exit(127);
        }
        std::string captured;
        if (!wait_prompt(terminal.master, "Phone number", captured)) {
            ::kill(child, SIGKILL); ::waitpid(child, nullptr, 0);
            throw std::runtime_error("CLI did not reach login prompt");
        }
        const auto competing_errors = directory / "competing-stderr";
        const auto competing_output = directory / "competing-stdout";
        const auto competing = ::fork();
        require(competing >= 0);
        if (competing == 0) {
            const int out = ::open(competing_output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            const int err = ::open(competing_errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            ::dup2(out, STDOUT_FILENO);
            ::dup2(err, STDERR_FILENO);
            ::execl(program, program, "auth", "status", config.c_str(), nullptr);
            ::_exit(127);
        }
        const auto competing_status = wait_child(competing);
        require(WIFEXITED(competing_status) && WEXITSTATUS(competing_status) == 1);
        require(read_file(competing_errors).find("already owned") != std::string::npos);
        require(read_file(competing_output).empty());
        ::kill(child, SIGTERM);
        const auto status = wait_child(child);
        require(WIFEXITED(status) && WEXITSTATUS(status) == 143);
        require(read_file(output).empty());
        auto diagnostics = read_file(errors);
        require(diagnostics.find("0123456789abcdef") == std::string::npos);
        require(diagnostics.find("shutdown_failed") == std::string::npos);
        const auto reopened = ::fork();
        require(reopened >= 0);
        if (reopened == 0) {
            const int out = ::open(output.c_str(), O_WRONLY | O_TRUNC);
            const int err = ::open(errors.c_str(), O_WRONLY | O_TRUNC);
            ::dup2(out, STDOUT_FILENO);
            ::dup2(err, STDERR_FILENO);
            ::execl(program, program, "auth", "status", config.c_str(), nullptr);
            ::_exit(127);
        }
        const auto reopened_status = wait_child(reopened);
        require(WIFEXITED(reopened_status) && WEXITSTATUS(reopened_status) == 1);
        diagnostics = read_file(errors);
        require(diagnostics.find("not authorized") != std::string::npos);
        require(diagnostics.find("already owned") == std::string::npos);
        require(diagnostics.find("shutdown_failed") == std::string::npos);
        require(read_file(output).empty());
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}

}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--qr") qr_rendering();
        else if (argc == 2) lifecycle(argv[1]);
        else { hidden_input(false); hidden_input(true); }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
