#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

namespace nb = nanobind;
using namespace ice;

NB_MODULE(icepy, module) {
    module.doc() = "Python access to the ICE canonical project model.";
    nb::class_<Project>(module, "Project")
        .def_prop_ro("name", [](const Project &project) { return project.name; })
        .def_prop_ro("identity", [](const Project &project) { return project.identity; })
        .def_prop_ro("schema_version", [](const Project &) { return Project::schemaVersion; })
        .def("validate", [](const Project &project) { return project.validate().size(); });
    module.def("load_project", [](const std::string &path) { return Project::load(path); });
    module.def("create_project", [](const std::string &path, const std::string &name) {
        Project project;
        project.identity = "project-" + name;
        project.name = name;
        project.save(path);
        return project;
    });
}
