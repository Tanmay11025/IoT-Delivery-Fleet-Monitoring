#pragma once

#include <filesystem>
#include <string>
#include <vector>

// One dashboard file held in memory and served at a fixed URL.
struct StaticFile {
    std::string url_path;      // e.g. "/dashboard/app.js"
    std::string content_type;
    std::string body;
};

// Load the dashboard's whitelisted files from `directory`.
//
// Only names in a fixed list are read, and each is registered as an exact
// route, so a request such as "/dashboard/../config.ini" can never reach the
// filesystem. index.html is also served at "/". Missing files are skipped and
// reported through `missing`.
std::vector<StaticFile> load_dashboard_files(const std::filesystem::path& directory,
                                             std::vector<std::string>& missing);
