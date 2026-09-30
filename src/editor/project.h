#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace andromeda::editor {

class ProjectManager;

class ProjectWriter {
public:
    ProjectWriter(const ProjectManager& project_manager, const std::filesystem::path& path);

    std::expected<void, std::string> write_header();

    std::expected<void, std::string> finalize();

private:
    static std::array<std::uint8_t, 2> u16_to_bytes(std::uint16_t num);
    static std::array<std::uint8_t, 4> u32_to_bytes(std::uint32_t num);

    std::expected<void, std::string> flush_buffer();
    std::expected<void, std::string> write(std::span<const std::uint8_t> data);

    static std::pair<std::uint32_t, std::vector<std::uint8_t>> text_to_bytes(std::string_view text);

    std::ofstream stream_;
    const ProjectManager& project_manager_;

    std::vector<std::uint8_t> buffer_;
};

}
