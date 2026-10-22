#pragma once

#include "icelib/core.hpp"

#include <filesystem>

namespace ice {

class LayoutInterchange {
  public:
    static void exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    static Project importGds(const std::filesystem::path &path);
};

} // namespace ice
