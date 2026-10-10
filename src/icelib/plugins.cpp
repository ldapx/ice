#include "icelib/plugins.hpp"

#include "icelib/core.hpp"

#include <toml++/toml.h>

namespace ice {
PluginManifest PluginManager::readManifest(const std::filesystem::path& root) {
    try {
        auto table = toml::parse_file((root / "plugin.toml").string());
        const auto* plugin = table["plugin"].as_table();
        if (!plugin) throw ProjectError("plugin.manifest", "Missing [plugin] table");
        PluginManifest manifest;
        manifest.name = (*plugin)["name"].value_or(std::string{});
        manifest.version = (*plugin)["version"].value_or(std::string{});
        manifest.iceTarget = (*plugin)["ice_target"].value_or(std::string{});
        manifest.root = root;
        if (manifest.name.empty() || manifest.version.empty() || manifest.iceTarget.empty())
            throw ProjectError("plugin.manifest", "Plugin name, version, and ice_target are required");
        if (const auto* permissions = table["permissions"].as_table()) {
            manifest.permissions.readProject = (*permissions)["read_project"].value_or(true);
            manifest.permissions.mutateProject = (*permissions)["mutate_project"].value_or(false);
            manifest.permissions.executeProcess = (*permissions)["execute_process"].value_or(false);
            manifest.permissions.readFilesystem = (*permissions)["read_filesystem"].value_or(false);
            manifest.permissions.writeFilesystem = (*permissions)["write_filesystem"].value_or(false);
        }
        return manifest;
    } catch (const ProjectError&) {
        throw;
    } catch (const std::exception& error) {
        throw ProjectError("plugin.manifest", error.what());
    }
}
void PluginManager::discover(const std::filesystem::path& root) {
    auto manifest = readManifest(root);
    const auto name = manifest.name;
    records_[name] = {std::move(manifest), PluginState::discovered, {}};
}
void PluginManager::initialize(const std::string& name) {
    auto it = records_.find(name);
    if (it == records_.end()) throw ProjectError("plugin.lifecycle", "Plugin was not discovered: " + name);
    auto& record = it->second;
    record.state = record.manifest.iceTarget == version() ? PluginState::initialized : PluginState::failed;
    if (record.state == PluginState::failed) record.error = "plugin targets a different ICE version";
}
void PluginManager::enable(const std::string& name) {
    auto it = records_.find(name);
    if (it == records_.end()) throw ProjectError("plugin.lifecycle", "Plugin was not discovered: " + name);
    auto& record = it->second;
    if (record.state != PluginState::initialized) throw ProjectError("plugin.lifecycle", "Plugin must be initialized first");
    record.state = PluginState::enabled;
}
void PluginManager::disable(const std::string& name) {
    auto it = records_.find(name);
    if (it == records_.end()) throw ProjectError("plugin.lifecycle", "Plugin was not discovered: " + name);
    auto& record = it->second;
    if (record.state != PluginState::enabled) throw ProjectError("plugin.lifecycle", "Plugin is not enabled");
    record.state = PluginState::disabled;
}
void PluginManager::unload(const std::string& name) {
    auto it = records_.find(name);
    if (it == records_.end()) throw ProjectError("plugin.lifecycle", "Plugin was not discovered: " + name);
    auto& record = it->second;
    record.state = PluginState::unloaded;
}
const PluginRecord& PluginManager::get(const std::string& name) const {
    auto it = records_.find(name);
    if (it == records_.end()) throw ProjectError("plugin.lifecycle", "Plugin was not discovered: " + name);
    return it->second;
}
const std::map<std::string, PluginRecord>& PluginManager::records() const noexcept { return records_; }
std::string pluginStateName(PluginState state) {
    switch (state) {
    case PluginState::discovered: return "discovered";
    case PluginState::loaded: return "loaded";
    case PluginState::initialized: return "initialized";
    case PluginState::enabled: return "enabled";
    case PluginState::disabled: return "disabled";
    case PluginState::unloaded: return "unloaded";
    case PluginState::failed: return "failed";
    }
    return "unknown";
}
} // namespace ice
