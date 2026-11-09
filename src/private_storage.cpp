#include "telegram_mcpp/private_storage.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace telegram_mcpp {
namespace {

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {
        if (value < 0) throw std::runtime_error("Cannot open private storage");
    }
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }

private:
    int value_;
};

void require_private(int descriptor, mode_t mode, bool directory) {
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != mode ||
        (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) ||
        (!directory && status.st_nlink != 1)) {
        throw std::runtime_error("Private storage has unsafe ownership, type, or permissions");
    }
}

Descriptor open_directory(const std::filesystem::path& path, bool create) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const auto& component : absolute.relative_path()) {
        if (component.empty()) continue;
        if (create && ::mkdirat(current.get(), component.c_str(), 0700) != 0 && errno != EEXIST) {
            throw std::runtime_error("Cannot create private directory");
        }
        current = Descriptor(::openat(current.get(), component.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    require_private(current.get(), 0700, true);
    return current;
}

}

void ensure_private_directory(const std::filesystem::path& path) {
    open_directory(path, true);
}

void create_private_file(const std::filesystem::path& path, std::string_view contents) {
    auto parent = open_directory(std::filesystem::absolute(path).parent_path(), true);
    Descriptor file(::openat(parent.get(), path.filename().c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (::fchmod(file.get(), 0600) != 0) throw std::runtime_error("Cannot protect private file");
    require_private(file.get(), 0600, false);
    while (!contents.empty()) {
        const auto count = ::write(file.get(), contents.data(), contents.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Cannot write private file");
        contents.remove_prefix(static_cast<std::size_t>(count));
    }
    if (::fsync(file.get()) != 0) throw std::runtime_error("Cannot persist private file");
}

std::string read_private_file(const std::filesystem::path& path) {
    auto parent = open_directory(std::filesystem::absolute(path).parent_path(), false);
    Descriptor file(::openat(parent.get(), path.filename().c_str(),
                             O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    require_private(file.get(), 0600, false);
    std::string contents;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("Cannot read configuration");
        if (count == 0) return contents;
        contents.append(buffer, static_cast<std::size_t>(count));
        if (contents.size() > 64 * 1024) throw std::runtime_error("Configuration exceeds 64 KiB");
    }
}

}
