#pragma once

#include <string>
#include <unordered_map>

// Tiny INI-style key=value config persistence.
// Lines starting with '#' or ';' are comments; whitespace around '=' is trimmed.
// Designed to live next to the plugin DLL so users can edit it manually.
class ConfigStore {
 public:
  // Load (or create empty) the config file at `filePath`. Always succeeds:
  // missing file == empty config.
  void Load(const std::string& filePath);

  // Persist current values back to the bound file.
  bool Save() const;

  // Bound file path (set by Load).
  const std::string& Path() const { return path_; }

  // Get a value, returning `defaultValue` if the key is absent.
  std::string GetString(const std::string& key, const std::string& defaultValue) const;
  double GetDouble(const std::string& key, double defaultValue) const;

  void SetString(const std::string& key, const std::string& value);
  void SetDouble(const std::string& key, double value);

  bool Has(const std::string& key) const { return values_.count(key) != 0; }

 private:
  std::string path_;
  std::unordered_map<std::string, std::string> values_;
};
