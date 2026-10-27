set_project("ice")
set_version("0.1.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

add_requires("spdlog", "nlohmann_json")

-- Agent Client Protocol support. Point AGENTSDK_CPP_DIR at an agentsdk-cpp
-- checkout, or keep the checkout next to the ICE repository.
local agentsdk_dir = os.getenv("AGENTSDK_CPP_DIR")
if not agentsdk_dir or not os.isdir(agentsdk_dir) then
    local sibling = path.join(os.projectdir(), "..", "agentsdk-cpp")
    if os.isdir(sibling) then
        agentsdk_dir = sibling
    end
end
local has_agentsdk = agentsdk_dir and os.isdir(path.join(agentsdk_dir, "src", "agentsdk"))
if has_agentsdk then
    agentsdk_dir = path.absolute(agentsdk_dir)
    add_requires("simdjson", "libcurl")
end

-- First-party warnings. Third-party headers are included via system paths, so
-- -Werror is intentionally not enabled.
local first_party_warnings = {"-Wall", "-Wextra", "-Wpedantic", "-Wno-missing-field-initializers"}

target("icelib")
    set_kind("static")
    add_files("src/icelib/*.cpp")
    add_includedirs("include", {public = true})
    add_cxxflags("-fPIC", {public = true})
    add_cxxflags(first_party_warnings)
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
    add_files("src/ice/*.cpp")
    add_deps("icelib")
    add_defines('ICE_SOURCE_DIR="' .. os.projectdir() .. '"')
    add_includedirs("include", "/usr/include/qt6", "/usr/include/qt6/QtCore",
                    "/usr/include/qt6/QtGui", "/usr/include/qt6/QtWidgets",
                    "/usr/include/qt6/QtSvg", "/usr/include/qt6/QtSvgWidgets")
    add_syslinks("Qt6Widgets", "Qt6Gui", "Qt6Core", "Qt6Svg", "Qt6SvgWidgets")
    add_cxxflags("-fPIC", {force = true})
    add_cxxflags(first_party_warnings)
    add_ldflags("-no-pie", {force = true})
    if has_agentsdk then
        add_includedirs(path.join(agentsdk_dir, "src"))
        add_files(path.join(agentsdk_dir, "src", "agentsdk", "**.cpp"))
        add_packages("spdlog", "simdjson", "libcurl")
        add_defines("ICE_HAVE_AGENTSDK=1")
        if is_plat("linux") then
            add_syslinks("pthread")
        elseif is_plat("windows") then
            add_syslinks("ws2_32", "wsock32")
        end
    end

target("ice-cli")
    set_kind("binary")
    add_files("src/ice-cli/main.cpp")
    add_deps("icelib")
    add_cxxflags(first_party_warnings)
    add_includedirs("include")

target("ice-pkg")
    set_kind("binary")
    add_files("src/ice-pkg/main.cpp")
    add_deps("icelib")
    add_cxxflags(first_party_warnings)
    add_includedirs("include")

target("ice-mcp")
    set_kind("binary")
    add_files("src/ice-mcp/main.cpp")
    add_deps("icelib")
    add_cxxflags(first_party_warnings)
    add_includedirs("include")
    add_packages("nlohmann_json")
    if has_agentsdk then
        add_includedirs(path.join(agentsdk_dir, "src"))
        add_files(path.join(agentsdk_dir, "src", "agentsdk", "common", "json_builder.cpp"),
                  path.join(agentsdk_dir, "src", "agentsdk", "mcp", "mcp_server.cpp"),
                  path.join(agentsdk_dir, "src", "agentsdk", "mcp", "mcp_json.cpp"),
                  path.join(agentsdk_dir, "src", "agentsdk", "mcp", "mcp_types.cpp"))
        add_packages("spdlog", "simdjson")
        add_defines("ICE_HAVE_AGENTSDK=1")
        if is_plat("linux") then
            add_syslinks("pthread")
        elseif is_plat("windows") then
            add_syslinks("ws2_32", "wsock32")
        end
    end

target("doctest")
    set_kind("binary")
    add_files("tests/*.cpp")
    add_deps("icelib")
    add_cxxflags(first_party_warnings)
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
