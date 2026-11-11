#pragma once

namespace ice::logging {

// Logger names used by the GUI message sinks. Keeping these in one place
// prevents the GUI, icelib, and verification paths from spelling categories
// differently.
inline constexpr const char *application = "ice.app";
inline constexpr const char *simulation = "ice.sim";
inline constexpr const char *implementation = "ice.impl";
inline constexpr const char *drc = "ice.drc";
inline constexpr const char *lvs = "ice.lvs";

} // namespace ice::logging
