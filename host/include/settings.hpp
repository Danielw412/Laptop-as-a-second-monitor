#pragma once
#include "platform.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
namespace bm {
std::filesystem::path settingsPath();
nlohmann::json loadSettings();
void saveSettings(const nlohmann::json &);
std::string displayIdentity(const Display &);
}
