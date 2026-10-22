#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ice {

enum class PluginState { discovered, loaded, initialized, enabled, disabled, unloaded, failed };

struct PermissionScope {
    bool readProject = true;
    bool mutateProject = false;
    bool executeProcess = false;
    bool readFilesystem = false;
    bool writeFilesystem = false;
};

struct PluginManifest {
    int schemaVersion = 1;
    std::string name;
    std::string version;
    std::string iceTarget;
    std::vector<std::string> capabilities;
    PermissionScope permissions;
    std::filesystem::path root;
};

struct PluginRecord {
    PluginManifest manifest;
    PluginState state = PluginState::discovered;
    std::string error;
};

class PluginManager {
  public:
    static PluginManifest readManifest(const std::filesystem::path &root);
    void discover(const std::filesystem::path &root);
    void initialize(const std::string &name);
    void enable(const std::string &name);
    void disable(const std::string &name);
    void unload(const std::string &name);
    const PluginRecord &get(const std::string &name) const;
    const std::map<std::string, PluginRecord> &records() const noexcept;

  private:
    std::map<std::string, PluginRecord> records_;
};

std::string pluginStateName(PluginState state);

} // namespace ice
