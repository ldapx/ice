#pragma once

#include "icelib/core.hpp"

#include <filesystem>

namespace ice {

struct InterchangeOptions {
    int schemaVersion = 1;
    bool preserveUnsupported = false;
};

class LayoutInterchange {
public:
    static void exportGdsSubset(const Project& project, ObjectId cellId,
                                const std::filesystem::path& path,
                                const InterchangeOptions& options = {});
    static Project importGdsSubset(const std::filesystem::path& path,
                                   const InterchangeOptions& options = {});
};

} // namespace ice
