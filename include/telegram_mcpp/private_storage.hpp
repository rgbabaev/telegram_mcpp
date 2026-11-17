#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace telegram_mcpp {

void ensure_private_directory(const std::filesystem::path& path);
void create_private_file(const std::filesystem::path& path, std::string_view contents);
std::string read_private_file(const std::filesystem::path& path);

class SessionLock {
public:
    explicit SessionLock(const std::filesystem::path& directory);
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
