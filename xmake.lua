set_project("ice")
set_version("0.1.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

add_requires("spdlog", "nlohmann_json")

target("icelib")
    set_kind("static")
    add_files("src/icelib/*.cpp")
    add_includedirs("include", {public = true})
    add_cxxflags("-fPIC", {public = true})
    -- Keep warnings on first-party code; third-party headers are included
    -- via system paths so -Werror is intentionally not enabled.
    -- Missing-field-init is intentional for aggregate construction of domain structs.
    add_cxxflags("-Wall", "-Wextra", "-Wpedantic", "-Wno-missing-field-initializers")
    add_packages("spdlog", "nlohmann_json")
    -- System-provided dependencies without xmake packages (Debian/Ubuntu layout).
    -- TODO: replace hardcoded /usr paths with packaged deps or pkg-config checks.
    add_includedirs("/usr/include/toml++", "/usr/include")
    add_linkdirs("/usr/lib")
    add_links("crypto")
    add_includedirs("/usr/include/ngspice")
    add_links("ngspice")

target("ice")
    set_kind("binary")
    add_files("src/ice/main.cpp")
    add_deps("icelib")
    add_defines('ICE_SOURCE_DIR="' .. os.projectdir() .. '"')
    add_includedirs("include", "/usr/include/qt6", "/usr/include/qt6/QtCore",
                    "/usr/include/qt6/QtGui", "/usr/include/qt6/QtWidgets",
                    "/usr/include/qt6/QtSvg", "/usr/include/qt6/QtSvgWidgets")
    add_syslinks("Qt6Widgets", "Qt6Gui", "Qt6Core", "Qt6Svg", "Qt6SvgWidgets")
    add_cxxflags("-fPIC", {force = true})
    add_ldflags("-no-pie", {force = true})

target("ice-cli")
    set_kind("binary")
    add_files("src/ice-cli/main.cpp")
    add_deps("icelib")
    add_includedirs("include")

target("ice-pkg")
    set_kind("binary")
    add_files("src/ice-pkg/main.cpp")
    add_deps("icelib")
    add_includedirs("include")

target("ice-mcp")
    set_kind("binary")
    add_files("src/ice-mcp/main.cpp")
    add_deps("icelib")
    add_includedirs("include")

target("doctest")
    set_kind("binary")
    add_files("tests/*.cpp")
    add_deps("icelib")
    add_includedirs("include")
    add_defines('ICE_SOURCE_DIR="' .. os.projectdir() .. '"')

target("icepy")
    set_kind("shared")
    set_prefixname("")
    add_files("src/icepy/module.cpp", "/usr/lib/python3.14/site-packages/nanobind/src/nb_combined.cpp")
    add_deps("icelib")
    add_includedirs("include", "/usr/include/python3.14",
                    "/usr/lib/python3.14/site-packages/nanobind/include",
                    "/usr/lib/python3.14/site-packages/nanobind/src")
    add_links("python3.14", "dl")

task("test")
    on_run(function ()
        os.exec("xmake build doctest")
        os.exec("xmake run doctest")
    end)
    set_menu {usage = "Run the doctest-compatible icelib test target"}

task("format")
    on_run(function ()
        for _, file in ipairs(os.files("include/**.hpp|src/**.cpp|tests/**.cpp")) do
            os.exec("clang-format -i " .. file)
        end
    end)

task("docs")
    on_run(function ()
        os.exec("sphinx-build -b html docs docs/_build")
    end)
    set_menu {usage = "Build the Sphinx documentation site"}
