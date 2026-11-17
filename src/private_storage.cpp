#include "telegram_mcpp/private_storage.hpp"

#include <cerrno>
#include <dirent.h>
#include <sys/file.h>
#include <sys/random.h>
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

void create_private_directory(int parent, const std::filesystem::path& name) {
    if (::mkdirat(parent, name.c_str(), 0700) == 0) {
        if (::fchmodat(parent, name.c_str(), 0700, AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::runtime_error("Cannot protect private directory");
        }
    } else if (errno != EEXIST) {
        throw std::runtime_error("Cannot create private directory");
    }
}

Descriptor open_directory(const std::filesystem::path& path, bool create) {
    const auto absolute = std::filesystem::absolute(path);
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const auto& component : absolute.relative_path()) {
        if (component.empty()) continue;
        if (create) create_private_directory(current.get(), component);
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

namespace telegram_mcpp {
namespace {

void inspect_private_tree(int directory, int depth, std::size_t& count) {
    if (depth > 32) throw std::runtime_error("Session directory nesting exceeds safety limit");
    const int copy = ::dup(directory);
    if (copy < 0) throw std::runtime_error("Cannot inspect session directory");
    std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(copy), ::closedir);
    if (!entries) { ::close(copy); throw std::runtime_error("Cannot inspect session directory"); }
    for (;;) {
        errno = 0;
        const auto entry = ::readdir(entries.get());
        if (!entry) {
            if (errno) throw std::runtime_error("Cannot inspect session directory");
            break;
        }
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (++count > 100000) throw std::runtime_error("Session file count exceeds safety limit");
        struct stat status {};
        if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::runtime_error("Cannot inspect session file");
        }
        if (S_ISDIR(status.st_mode)) {
            Descriptor child(::openat(directory, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            require_private(child.get(), 0700, true);
            inspect_private_tree(child.get(), depth + 1, count);
        } else {
            Descriptor file(::openat(directory, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
            require_private(file.get(), 0600, false);
        }
    }
}

std::string random_key() {
    std::string key(32, '\0');
    std::size_t position = 0;
    while (position < key.size()) {
        const auto count = ::getrandom(key.data() + position, key.size() - position, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Cannot generate database key");
        position += static_cast<std::size_t>(count);
    }
    return key;
}

}

class PrivateUmask {
public:
    PrivateUmask() : previous_(::umask(0077)) {}
    ~PrivateUmask() { ::umask(previous_); }
private:
    mode_t previous_;
};

struct SessionLock::Implementation {
    PrivateUmask private_umask;
    Descriptor directory;
    Descriptor file;
    explicit Implementation(const std::filesystem::path& path)
        : directory(open_directory(path, true)),
          file(::openat(directory.get(), ".session.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)) {
        require_private(file.get(), 0600, false);
        if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) throw std::runtime_error("TDLib state directory is already owned by another process");
            throw std::runtime_error("Cannot lock TDLib state directory");
        }
        std::size_t count = 0;
        inspect_private_tree(directory.get(), 0, count);
        const auto key = path / ".database.key";
        if (!std::filesystem::exists(key)) {
            const auto database = path / "database";
            if (std::filesystem::exists(database) && !std::filesystem::is_empty(database)) {
                throw std::runtime_error("Database key is missing; restore the private session backup");
            }
            create_private_file(key, random_key());
        }
        if (read_private_file(key).size() != 32) throw std::runtime_error("Invalid database key file");
        ensure_private_directory(path / "database");
        ensure_private_directory(path / "files");
    }
};

SessionLock::SessionLock(const std::filesystem::path& directory)
    : implementation_(std::make_unique<Implementation>(directory)) {}
SessionLock::~SessionLock() = default;

}
