#include "resource_path.hpp"

#include <filesystem>
#include <system_error>

std::string find_resource(const std::string& rel) {
    std::error_code ec;
    for (const std::string& c : { rel, "../" + rel })
        if (std::filesystem::is_regular_file(std::filesystem::u8path(c), ec)) return c;
    return {};
}
