#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "ice_openroad/heatmap.hpp"
#include "ice_openroad/types.hpp"

namespace ice::openroad {

struct EngineOptions {
    std::filesystem::path log_file;
    std::filesystem::path metrics_file;
    int threads = 1;
};

class Engine {
  public:
    Engine();
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    bool initialize(const EngineOptions &options, std::string &error);
    void shutdown();

    StageResult runStage(const StageJob &job, const std::function<void(const std::string &)> &log = {});
    HeatMapResult exportHeatMap(const HeatMapJob &job);

    static std::string version();
    static std::string openroadVersion();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ice::openroad
