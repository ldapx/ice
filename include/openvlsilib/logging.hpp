#pragma once

namespace openvlsi::logging {

// Logger names used by the GUI message sinks. Keeping these in one place
// prevents the GUI, openvlsilib, and verification paths from spelling categories
// differently.
inline constexpr const char *application = "openvlsi.app";
inline constexpr const char *simulation = "openvlsi.sim";
inline constexpr const char *implementation = "openvlsi.impl";
inline constexpr const char *drc = "openvlsi.drc";
inline constexpr const char *lvs = "openvlsi.lvs";

} // namespace openvlsi::logging
