#include "static_files.h"

#include <array>
#include <fstream>
#include <sstream>

namespace {
struct DashboardAsset {
    const char* name;
    const char* content_type;
};

constexpr std::array<DashboardAsset, 6> dashboard_assets{{
    {"index.html", "text/html; charset=utf-8"},
    {"style.css", "text/css; charset=utf-8"},
    {"app.js", "text/javascript; charset=utf-8"},
    {"charts.js", "text/javascript; charset=utf-8"},
    {"fleet_map.js", "text/javascript; charset=utf-8"},
    {"city_map.json", "application/json"},
}};
}  // namespace

std::vector<StaticFile> load_dashboard_files(const std::filesystem::path& directory,
                                             std::vector<std::string>& missing) {
    std::vector<StaticFile> files;
    for (const DashboardAsset& asset : dashboard_assets) {
        std::ifstream input(directory / asset.name, std::ios::binary);
        if (!input) {
            missing.emplace_back(asset.name);
            continue;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        const std::string body = contents.str();
        files.push_back(StaticFile{std::string("/dashboard/") + asset.name,
                                   asset.content_type, body});
        if (std::string(asset.name) == "index.html") {
            files.push_back(StaticFile{"/", asset.content_type, body});
        }
    }
    return files;
}
