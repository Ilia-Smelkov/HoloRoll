#include "core/config_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::string Trim(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  std::size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}
}  // namespace

void ConfigStore::Load(const std::string& filePath) {
  path_ = filePath;
  values_.clear();

  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, filePath.c_str(), "r") != 0 || !file) {
    return;  // missing config is fine; values stay empty.
  }
#else
  file = std::fopen(filePath.c_str(), "r");
  if (!file) return;
#endif

  char line[2048];
  while (std::fgets(line, sizeof(line), file)) {
    std::string l = Trim(line);
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    const auto eq = l.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(l.substr(0, eq));
    const std::string val = Trim(l.substr(eq + 1));
    if (key.empty()) continue;
    values_[key] = val;
  }

  std::fclose(file);
}

bool ConfigStore::Save() const {
  if (path_.empty()) return false;

  std::FILE* file = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&file, path_.c_str(), "w") != 0 || !file) {
    return false;
  }
#else
  file = std::fopen(path_.c_str(), "w");
  if (!file) return false;
#endif

  std::fputs("# REAPER MDD Viewport configuration.\n", file);
  std::fputs("# Edit and save, then use 'Reload config' in the viewport.\n", file);
  std::fputs("# Lines starting with '#' or ';' are comments.\n\n", file);

  for (const auto& kv : values_) {
    std::fputs(kv.first.c_str(), file);
    std::fputc('=', file);
    std::fputs(kv.second.c_str(), file);
    std::fputc('\n', file);
  }

  std::fclose(file);
  return true;
}

std::string ConfigStore::GetString(const std::string& key, const std::string& defaultValue) const {
  const auto it = values_.find(key);
  return it == values_.end() ? defaultValue : it->second;
}

double ConfigStore::GetDouble(const std::string& key, double defaultValue) const {
  const auto it = values_.find(key);
  if (it == values_.end()) return defaultValue;
  try {
    return std::stod(it->second);
  } catch (...) {
    return defaultValue;
  }
}

void ConfigStore::SetString(const std::string& key, const std::string& value) {
  values_[key] = value;
}

void ConfigStore::SetDouble(const std::string& key, double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%g", value);
  values_[key] = buf;
}
