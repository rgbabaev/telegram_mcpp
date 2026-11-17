#include "telegram_mcpp/operator_terminal.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace telegram_mcpp {
namespace {

class HiddenInput {
public:
    explicit HiddenInput(int descriptor) : descriptor_(descriptor) {
        if (::tcgetattr(descriptor_, &previous_) != 0) throw std::runtime_error("Cannot protect terminal input");
        auto hidden = previous_;
        hidden.c_lflag &= ~(ECHO | ECHONL);
        if (::tcsetattr(descriptor_, TCSAFLUSH, &hidden) != 0) throw std::runtime_error("Cannot protect terminal input");
    }
    ~HiddenInput() { ::tcsetattr(descriptor_, TCSAFLUSH, &previous_); }
private:
    int descriptor_;
    termios previous_{};
};

void write_all(int descriptor, const std::string& text) {
    std::size_t position = 0;
    while (position < text.size()) {
        const auto count = ::write(descriptor, text.data() + position, text.size() - position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Cannot write operator terminal");
        position += static_cast<std::size_t>(count);
    }
}

}

OperatorTerminal::OperatorTerminal(Cancelled cancelled)
    : terminal_(::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC)), cancelled_(std::move(cancelled)) {
    if (terminal_ < 0 || !::isatty(terminal_)) {
        if (terminal_ >= 0) ::close(terminal_);
        throw std::runtime_error("auth login requires an interactive terminal");
    }
}

OperatorTerminal::~OperatorTerminal() { ::close(terminal_); }
void OperatorTerminal::write(const std::string& value) { write_all(terminal_, value); }

std::string OperatorTerminal::secret(const std::string& prompt) {
    HiddenInput hidden(terminal_);
    write(prompt);
    std::string value;
    for (;;) {
        if (cancelled_ && cancelled_()) { write("\n"); throw AccountError(AccountErrorCode::cancelled); }
        pollfd descriptor{terminal_, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            throw AccountError(AccountErrorCode::cancelled);
        }
        if (!ready) continue;
        char buffer[256];
        const auto count = ::read(terminal_, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw AccountError(AccountErrorCode::cancelled);
        for (ssize_t index = 0; index < count; ++index) {
            if (buffer[index] == '\n' || buffer[index] == '\r') {
                write("\n");
                if (value.empty()) throw AccountError(AccountErrorCode::invalid_input);
                return value;
            }
            value += buffer[index];
            if (value.size() > 1024) throw AccountError(AccountErrorCode::invalid_input);
        }
    }
}

void OperatorTerminal::qr(const std::string& link) {
    if (link.rfind("tg://login?token=", 0) != 0 || link.size() > 2048) {
        throw AccountError(AccountErrorCode::unavailable);
    }
    int pipe[2];
    if (::pipe2(pipe, O_CLOEXEC) != 0) throw std::runtime_error("Cannot render login QR code");
    try { write_all(pipe[1], link); }
    catch (...) { ::close(pipe[0]); ::close(pipe[1]); throw; }
    ::close(pipe[1]);
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipe[0]);
        throw std::runtime_error("Cannot render login QR code");
    }
    const auto cleanup = [&] { ::posix_spawn_file_actions_destroy(&actions); ::close(pipe[0]); };
    const bool failed = ::posix_spawn_file_actions_adddup2(&actions, pipe[0], STDIN_FILENO) != 0 ||
        ::posix_spawn_file_actions_adddup2(&actions, terminal_, STDOUT_FILENO) != 0 ||
        ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0;
    char command[] = "qrencode";
    char type[] = "-t";
    char utf8[] = "ANSIUTF8";
    char* arguments[] = {command, type, utf8, nullptr};
    pid_t child = -1;
    const auto result = failed ? -1 : ::posix_spawnp(&child, command, &actions, nullptr, arguments, environ);
    cleanup();
    if (result != 0) throw std::runtime_error("Install qrencode for QR login, or rerun auth login with --phone");
    int status = 0;
    for (;;) {
        const auto finished = ::waitpid(child, &status, WNOHANG);
        if (finished == child) break;
        if (finished < 0 && errno != EINTR) throw std::runtime_error("QR renderer failed");
        if (cancelled_ && cancelled_()) {
            ::kill(child, SIGTERM);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw AccountError(AccountErrorCode::cancelled);
        }
        ::poll(nullptr, 0, 20);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("QR renderer failed; rerun auth login with --phone");
    write("Scan this QR code using Telegram Settings > Devices > Link Desktop Device. Ctrl-C cancels.\n");
}

void OperatorTerminal::rejected() { write("Input rejected; try again.\n"); }

}
