set_project("ice")
set_version("0.1.0")
set_languages("c++20")
add_rules("mode.debug", "mode.release")

-- XPack provides `xmake pack` for NSIS installers and archive releases.
includes("@builtin/xpack")

local is_windows = is_plat("windows", "mingw", "msys", "cygwin")
local is_msvc = is_plat("windows")
local is_macos = is_plat("macosx")
local is_linux = is_plat("linux")

local function env_bool(name, default)
    local value = os.getenv(name)
    if value == nil or value == "" then
        return default
    end
    value = value:lower()
    return value ~= "0" and value ~= "false" and value ~= "off" and value ~= "no"
end

-- Project scripts run in a restricted Lua sandbox, so package discovery is
-- done by looking for pkg-config metadata in the standard locations. Set
-- PKG_CONFIG_PATH/MINGW_PREFIX when dependencies live elsewhere.
local function split_path_list(value, separator)
    local result = {}
    if not value or value == "" then
        return result
    end
    for entry in value:gmatch("[^" .. separator .. "]+") do
        if entry ~= "" then
            table.insert(result, entry)
        end
    end
    return result
end

local function append_path_list(dirs, value, defaultSeparator)
    if not value or value == "" then
        return
    end
    local separator = defaultSeparator
    if value:find(";", 1, true) then
        separator = ";"
    elseif is_host("windows") and value:match("^%a:[/\\]") then
        -- A single Windows path such as C:\msys64\ucrt64\lib\pkgconfig.
        table.insert(dirs, value)
        return
    end
    for _, entry in ipairs(split_path_list(value, separator)) do
        table.insert(dirs, entry)
    end
end

local function pkg_search_dirs()
    local dirs = {}
    local separator = is_host("windows") and ";" or ":"
    append_path_list(dirs, os.getenv("ICE_PKG_CONFIG_DIR"), separator)
    append_path_list(dirs, os.getenv("PKG_CONFIG_PATH"), separator)
    append_path_list(dirs, os.getenv("PKG_CONFIG_LIBDIR"), separator)
    if is_host("windows") then
        local prefix = os.getenv("MINGW_PREFIX") or os.getenv("MSYSTEM_PREFIX")
        if prefix and prefix ~= "" then
            table.insert(dirs, path.join(prefix, "lib", "pkgconfig"))
            table.insert(dirs, path.join(prefix, "share", "pkgconfig"))
        end
        for _, prefix in ipairs({"C:/msys64/ucrt64", "C:/msys64/mingw64", "C:/msys64/clang64"}) do
            table.insert(dirs, path.join(prefix, "lib", "pkgconfig"))
            table.insert(dirs, path.join(prefix, "share", "pkgconfig"))
        end
    elseif is_host("macosx") then
        for _, prefix in ipairs({"/opt/homebrew", "/usr/local"}) do
            table.insert(dirs, path.join(prefix, "lib", "pkgconfig"))
            table.insert(dirs, path.join(prefix, "share", "pkgconfig"))
            table.insert(dirs, path.join(prefix, "opt", "openssl@3", "lib", "pkgconfig"))
        end
    else
        for _, dir in ipairs({
            "/usr/lib/pkgconfig",
            "/usr/lib64/pkgconfig",
            "/usr/lib/x86_64-linux-gnu/pkgconfig",
            "/usr/share/pkgconfig",
            "/usr/local/lib/pkgconfig",
            "/usr/local/share/pkgconfig",
        }) do
            table.insert(dirs, dir)
        end
        for _, dir in ipairs(os.dirs("/usr/lib/*/pkgconfig")) do
            table.insert(dirs, dir)
        end
    end
    return dirs
end

local pkg_dirs = pkg_search_dirs()

local function pkg_available(name)
    for _, dir in ipairs(pkg_dirs) do
        if os.isfile(path.join(dir, name .. ".pc")) then
            return true
        end
    end
    return false
end

local function find_pkg(names)
    for _, name in ipairs(names) do
        if pkg_available(name) then
            return "pkgconfig::" .. name
        end
    end
    return nil
end

local function append_package(list, package)
    if package then
        table.insert(list, package)
    end
end

-- Opt-in toggles. OpenROAD remains Linux-only by default because its helper is
-- built with Bazel and the Bash build script; other platforms still build all
-- other ICE components. GUI and Python targets are skipped when their SDKs are
-- not available.
local enable_openroad = env_bool("ICE_ENABLE_OPENROAD", is_linux)
local build_gui = env_bool("ICE_BUILD_GUI", true)

-- Bake a small build fingerprint into icelib so MCP clients can detect a
-- long-lived server that was started before the last install.
--
-- The fingerprint must be stable across no-op xmake invocations. Deriving the
-- timestamp from the clock here makes every `xmake run/build` look like a
-- configuration change, invalidating all cached objects. Use the newest
-- first-party source timestamp instead, and fall back to the clock only when
-- no source files are available.
-- Set ICE_BUILD_COMMIT/ICE_BUILD_TIME explicitly for reproducible packaging.
local build_commit = os.getenv("ICE_BUILD_COMMIT") or "unknown"

local build_time = os.getenv("ICE_BUILD_TIME")
if not build_time then
    local latest_mtime = 0
    local fingerprint_files = {
        "xmake.lua",
        "src/**.cpp",
        "src/**.hpp",
        "include/**.hpp",
        "tests/**.cpp",
    }
    for _, pattern in ipairs(fingerprint_files) do
        for _, file in ipairs(os.files(pattern)) do
            local mtime = os.mtime(file)
            if mtime and mtime > latest_mtime then
                latest_mtime = mtime
            end
        end
    end
    if latest_mtime == 0 then
        latest_mtime = os.time()
    end
    build_time = os.date("!%Y-%m-%dT%H:%M:%SZ", latest_mtime)
end
local build_type = is_mode("release") and "release" or "debug"

local function define_string(name, value)
    value = tostring(value):gsub("\\", "\\\\"):gsub('"', '\\"')
    return name .. '="' .. value .. '"'
end

local source_define = {define_string("ICE_SOURCE_DIR", os.projectdir())}
local build_defines = {
    define_string("ICE_BUILD_COMMIT", build_commit),
    define_string("ICE_BUILD_TIME", build_time),
    define_string("ICE_BUILD_TYPE", build_type),
}

-- Agent Client Protocol support. The normal build uses the bundled
-- third_party/agentsdk-cpp submodule. AGENTSDK_CPP_DIR and a sibling checkout
-- remain supported for downstream packaging.
local agentsdk_dir = os.getenv("AGENTSDK_CPP_DIR")
if not agentsdk_dir or not os.isdir(agentsdk_dir) then
    local bundled = path.join(os.projectdir(), "third_party", "agentsdk-cpp")
    if os.isdir(bundled) then
        agentsdk_dir = bundled
    end
end
if not agentsdk_dir or not os.isdir(agentsdk_dir) then
    local sibling = path.join(os.projectdir(), "..", "agentsdk-cpp")
    if os.isdir(sibling) then
        agentsdk_dir = sibling
    end
end
local has_agentsdk = agentsdk_dir and os.isdir(path.join(agentsdk_dir, "src", "agentsdk"))
if has_agentsdk then
    agentsdk_dir = path.absolute(agentsdk_dir)
end

-- Dependencies are resolved through pkg-config on every platform. This keeps
-- Linux, macOS (Homebrew), and Windows (MSYS2 UCRT64) on the same code path.
-- Set PKG_CONFIG_PATH to the prefix that provides the .pc files when the
-- dependency is installed outside the compiler's default search path.
local core_packages = {}
local fallback_includedirs = {}
local fallback_linkdirs = {}
local fallback_links = {}

-- Header-only packages fall back to xmake's package manager when the host
-- does not provide pkg-config metadata. This keeps fresh CI images and local
-- setups buildable without manual header installation.
local spdlog_package = find_pkg({"spdlog"})
if spdlog_package then
    append_package(core_packages, spdlog_package)
else
    table.insert(core_packages, "spdlog")
end
local nlohmann_package = find_pkg({"nlohmann_json"})
if nlohmann_package then
    append_package(core_packages, nlohmann_package)
else
    table.insert(core_packages, "nlohmann_json")
end
local toml_package = find_pkg({"tomlplusplus"})
if toml_package then
    append_package(core_packages, toml_package)
else
    table.insert(core_packages, "toml++")
end

local openssl_package = find_pkg({"openssl"})
append_package(core_packages, openssl_package)
if not openssl_package then
    local prefix = os.getenv("ICE_OPENSSL_PREFIX")
    if prefix and os.isdir(prefix) then
        table.insert(fallback_includedirs, path.join(prefix, "include"))
        table.insert(fallback_linkdirs, path.join(prefix, "lib"))
    end
    table.insert(fallback_links, "ssl")
    table.insert(fallback_links, "crypto")
end

local zlib_package = find_pkg({"zlib"})
append_package(core_packages, zlib_package)
if not zlib_package then
    table.insert(fallback_links, "z")
end

local ngspice_package = find_pkg({"ngspice", "libngspice"})
append_package(core_packages, ngspice_package)
if not ngspice_package then
    local candidates = {}
    local configured = os.getenv("ICE_NGSPICE_PREFIX")
    if configured and configured ~= "" then
        table.insert(candidates, configured)
    end
    if is_host("windows") then
        local prefix = os.getenv("MINGW_PREFIX") or os.getenv("MSYSTEM_PREFIX")
        if prefix and prefix ~= "" then
            table.insert(candidates, prefix)
        end
    end
    for _, prefix in ipairs({"/opt/homebrew/opt/libngspice", "/usr/local/opt/libngspice", "/opt/homebrew",
                             "/usr/local"}) do
        table.insert(candidates, prefix)
    end
    for _, prefix in ipairs(candidates) do
        if os.isfile(path.join(prefix, "include", "ngspice", "sharedspice.h")) then
            table.insert(fallback_includedirs, path.join(prefix, "include"))
            table.insert(fallback_linkdirs, path.join(prefix, "lib"))
            table.insert(fallback_links, "ngspice")
            break
        end
    end
end

-- The source-built xmake package is used instead of distro pkg-config
-- metadata, which can point at unusable LTO stubs on some Linux systems.
table.insert(core_packages, "clipper2")

local function apply_fallbacks()
    for _, directory in ipairs(fallback_includedirs) do
        add_includedirs(directory)
    end
    for _, directory in ipairs(fallback_linkdirs) do
        add_linkdirs(directory)
    end
    for _, link in ipairs(fallback_links) do
        add_links(link)
    end
end

local agent_packages = {}
if has_agentsdk then
    local curl_package = find_pkg({"libcurl"})
    append_package(agent_packages, curl_package or "libcurl")
    local simdjson_package = find_pkg({"simdjson"})
    append_package(agent_packages, simdjson_package or "simdjson")
end

local qt_packages = {}
local qt_framework_dir
local qt_framework_includes = {}
local qt_framework_names = {}
local has_qt = pkg_available("Qt6Widgets") and pkg_available("Qt6Svg") and pkg_available("Qt6SvgWidgets")
if build_gui and has_qt then
    append_package(qt_packages, find_pkg({"Qt6Widgets"}))
    append_package(qt_packages, find_pkg({"Qt6Svg"}))
    append_package(qt_packages, find_pkg({"Qt6SvgWidgets"}))
elseif build_gui then
    -- The official Qt macOS packages are framework builds without .pc
    -- files. Accept an explicit SDK prefix and link the frameworks directly.
    local prefix = os.getenv("ICE_QT_PREFIX")
    if prefix and os.isdir(prefix) then
        local framework_dir = path.join(prefix, "lib")
        local modules = {"QtCore", "QtGui", "QtWidgets", "QtSvg", "QtSvgWidgets"}
        local complete = true
        local includes = {}
        for _, module in ipairs(modules) do
            local headers = path.join(framework_dir, module .. ".framework", "Headers")
            if os.isdir(headers) then
                table.insert(includes, headers)
            else
                complete = false
            end
        end
        if complete then
            has_qt = true
            qt_framework_dir = framework_dir
            qt_framework_includes = includes
            qt_framework_names = modules
        end
    end
end
if build_gui and not has_qt then
    print("warning: Qt 6 (Widgets, Svg, SvgWidgets) was not found; skipping the ice GUI target")
    build_gui = false
end

for _, package in ipairs(core_packages) do
    add_requires(package)
end
for _, package in ipairs(agent_packages) do
    add_requires(package)
end
for _, package in ipairs(qt_packages) do
    add_requires(package)
end

-- First-party warnings. Third-party headers are included via system paths, so
-- -Werror is intentionally not enabled.
local first_party_warnings
if is_msvc then
    first_party_warnings = {"/W3", "/utf-8", "/permissive-"}
else
    first_party_warnings = {"-Wall", "-Wextra", "-Wpedantic", "-Wno-missing-field-initializers"}
end

-- MSYS2 UCRT64 GCC can be invoked from outside its shell without resolving the
-- target C headers. Add the sysroot include after the built-in dirs so
-- <cstdlib>'s #include_next <stdlib.h> still finds it.
local toolchain_cxxflags = {}
local mingw_sysroot = os.getenv("ICE_MINGW_SYSROOT")
if is_windows and mingw_sysroot and mingw_sysroot ~= "" then
    table.insert(toolchain_cxxflags, "-idirafter")
    table.insert(toolchain_cxxflags, path.join(mingw_sysroot, "x86_64-w64-mingw32", "include"))
end
local function apply_toolchain_flags()
    if #toolchain_cxxflags > 0 then
        add_cxxflags(toolchain_cxxflags)
    end
end

-- The C++ OpenROAD helper is required by the physical implementation flow
-- and the ODB-backed previews. It is Linux-only in the default build because
-- its pinned upstream toolchain is built with Bazel and Bash.
if enable_openroad then
    target("ice-openroad")
        set_kind("phony")
        on_build(function (target)
            -- A phony target is never up to date, so this would otherwise re-run the
            -- full OpenROAD Bazel build on every `xmake build/run ice`. Skip it once
            -- the helper is installed; `xmake build -r ice-openroad` still forces a
            -- rebuild.
            if os.isfile(path.join(target:targetdir(), "ice-openroad")) then
                return
            end
            os.execv("bash", {
                path.join(os.projectdir(), "scripts", "build-ice-openroad.sh"),
                "--output-dir", target:targetdir()
            })
        end)
end

target("icelib")
    set_kind("static")
    add_files("src/icelib/*.cpp")
    add_includedirs("include", {public = true})
    add_defines(source_define, {public = true})
    add_defines(build_defines, {public = true})
    if not is_msvc then
        add_cxxflags("-fPIC", {public = true})
    end
    add_cxxflags(first_party_warnings)
    apply_toolchain_flags()
    apply_fallbacks()
    for _, package in ipairs(core_packages) do
        add_packages(package, {public = true})
    end
    if is_windows then
        add_syslinks("ws2_32", "wsock32", "crypt32", "advapi32", "user32")
    elseif is_linux then
        add_syslinks("dl", "pthread")
    end

if has_agentsdk then
    target("agentsdk")
        set_kind("static")
        set_languages("c++20")
        add_files(path.join(agentsdk_dir, "src", "agentsdk", "**.cpp"))
        -- The A2A HTTP server is POSIX-socket-only and is not used by ICE.
        -- ACP and MCP remain available on every platform.
        remove_files(path.join(agentsdk_dir, "src", "agentsdk", "a2a", "server.cpp"))
        remove_files(path.join(agentsdk_dir, "src", "agentsdk", "a2a", "http", "http_listener.cpp"))
        remove_files(path.join(agentsdk_dir, "src", "agentsdk", "a2a", "http", "http_server.cpp"))
        remove_files(path.join(agentsdk_dir, "src", "agentsdk", "a2a", "http", "json_rpc_server.cpp"))
        add_includedirs(path.join(agentsdk_dir, "src"), {public = true})
        for _, package in ipairs(agent_packages) do
            add_packages(package, {public = true})
        end
        -- spdlog is public already through icelib for ICE targets, but the
        -- standalone SDK also needs it when built on its own.
        for _, package in ipairs(core_packages) do
            if package:find("spdlog") then
                add_packages(package, {public = true})
            end
        end
        if not is_msvc then
            add_cxxflags("-fPIC")
        end
        apply_toolchain_flags()
        if is_windows then
            add_syslinks("ws2_32", "wsock32", "crypt32", "advapi32", "user32")
        elseif is_linux then
            add_syslinks("pthread", "dl")
        end
end

if build_gui then
    target("ice")
        set_kind("binary")
        add_files("src/ice/*.cpp")
        if is_windows then
            add_files("assets/ice.rc")
        end
        add_deps("icelib")
        if enable_openroad then
            add_deps("ice-openroad")
        end
        add_defines(source_define)
        add_cxxflags(first_party_warnings)
        apply_toolchain_flags()
        apply_fallbacks()
        for _, package in ipairs(core_packages) do
            add_packages(package)
        end
        for _, package in ipairs(qt_packages) do
            add_packages(package)
        end
        if qt_framework_dir then
            add_frameworkdirs(qt_framework_dir)
            for _, framework in ipairs(qt_framework_names) do
                add_frameworks(framework)
            end
            for _, include in ipairs(qt_framework_includes) do
                add_includedirs(include)
            end
        end
        add_installfiles("assets/icons/tabs/*.svg", {prefixdir = "share/ice/icons/tabs"})
        if is_linux then
            add_ldflags("-no-pie", {force = true})
            add_rpathdirs("$ORIGIN/../lib")
        elseif is_macos then
            add_rpathdirs("@loader_path/../lib")
        end
        if has_agentsdk then
            add_deps("agentsdk")
            add_defines("ICE_HAVE_AGENTSDK=1")
            for _, package in ipairs(agent_packages) do
                add_packages(package)
            end
        end
end

target("ice-cli")
    set_kind("binary")
    add_files("src/ice-cli/main.cpp")
    add_deps("icelib")
    if enable_openroad then
        add_deps("ice-openroad")
    end
    add_cxxflags(first_party_warnings)
    add_includedirs("include")
    add_defines(source_define)
    apply_toolchain_flags()
    apply_fallbacks()
    for _, package in ipairs(core_packages) do
        add_packages(package)
    end

target("ice-pkg")
    set_kind("binary")
    add_files("src/ice-pkg/main.cpp")
    add_deps("icelib")
    add_cxxflags(first_party_warnings)
    add_includedirs("include")
    apply_toolchain_flags()
    apply_fallbacks()
    for _, package in ipairs(core_packages) do
        add_packages(package)
    end

target("ice-mcp")
    set_kind("binary")
    add_files("src/ice-mcp/main.cpp")
    add_deps("icelib")
    if enable_openroad then
        add_deps("ice-openroad")
    end
    add_cxxflags(first_party_warnings)
    add_includedirs("include")
    add_defines(source_define)
    apply_toolchain_flags()
    apply_fallbacks()
    for _, package in ipairs(core_packages) do
        add_packages(package)
    end
    if has_agentsdk then
        add_deps("agentsdk")
        add_defines("ICE_HAVE_AGENTSDK=1")
        for _, package in ipairs(agent_packages) do
            add_packages(package)
        end
    end

-- The doctest suite uses POSIX process and environment APIs. Keep it on
-- Linux/macOS; Windows release builds still compile all application targets.
if not is_windows then
    target("doctest")
        set_kind("binary")
        add_files("tests/*.cpp")
        add_deps("icelib")
        add_cxxflags(first_party_warnings)
        add_includedirs("include")
        add_defines(source_define)
        apply_fallbacks()
        for _, package in ipairs(core_packages) do
            add_packages(package)
        end
end

-- Optional Python bindings. Set ICE_PYTHON_INCLUDE, ICE_PYTHON_LIBDIR,
-- ICE_PYTHON_LIB, and NANOBIND_ROOT (or install nanobind into the selected
-- Python) to enable this target on macOS/Windows as well.
local python_version = os.getenv("ICE_PYTHON_VERSION") or "3.14"
local python_include = os.getenv("ICE_PYTHON_INCLUDE") or ("/usr/include/python" .. python_version)
local python_libdir = os.getenv("ICE_PYTHON_LIBDIR")
local python_lib = os.getenv("ICE_PYTHON_LIB") or
                       (is_windows and ("python" .. python_version:gsub("%.", "")) or ("python" .. python_version))
local nanobind_root = os.getenv("NANOBIND_ROOT") or
                          ("/usr/lib/python" .. python_version .. "/site-packages/nanobind")
local has_python = os.isdir(python_include) and os.isdir(path.join(nanobind_root, "include")) and
                       os.isdir(path.join(nanobind_root, "src"))
local build_python = env_bool("ICE_BUILD_PYTHON", has_python)
if build_python and not has_python then
    print("warning: Python/nanobind headers were not found; skipping the icepy target")
    build_python = false
end

if build_python then
    target("icepy")
        set_kind("shared")
        set_prefixname("")
        add_files("src/icepy/module.cpp", path.join(nanobind_root, "src", "nb_combined.cpp"))
        add_deps("icelib")
        add_defines(source_define)
        add_includedirs("include", python_include, path.join(nanobind_root, "include"), path.join(nanobind_root, "src"))
        apply_toolchain_flags()
        apply_fallbacks()
        for _, package in ipairs(core_packages) do
            add_packages(package)
        end
        if python_libdir then
            add_linkdirs(python_libdir)
        end
        add_links(python_lib)
        if is_linux then
            add_links("dl")
        end
        add_installfiles("python/icepy_pcells.py", {prefixdir = "share/ice/python"})
        add_installfiles("python/icepy_sim.py", {prefixdir = "share/ice/python"})
        add_installfiles("assets/simulation/report_runner.py", {prefixdir = "share/ice/assets/simulation"})
        add_installfiles("assets/layout/procedural_runner.py", {prefixdir = "share/ice/assets/layout"})
        add_installfiles("assets/layout/generators/*.py", {prefixdir = "share/ice/assets/layout/generators"})
end

task("test")
    on_run(function ()
        if is_plat("windows", "mingw") then
            print("the doctest suite is not available on Windows")
            return
        end
        os.exec("xmake build doctest")
        os.exec("xmake run doctest")
    end)
    set_menu {usage = "Run the doctest-compatible icelib test target"}

task("test-python")
    on_run(function ()
        local python = os.getenv("ICE_PYTHON") or (is_host("windows") and "python" or "python3")
        local builddir = path.join(os.projectdir(), "build", os.host(), os.arch(), "release")
        os.exec(python .. " tests/mcp_tools_smoke.py")
        os.exec("env PYTHONPATH=" .. builddir .. ":python " .. python .. " tests/icepy_api_tests.py")
        os.exec("env PYTHONPATH=" .. builddir .. ":python " .. python .. " tests/icepy_smoke.py")
        os.exec("env PYTHONPATH=" .. builddir .. ":python " .. python .. " tests/icepy_sim_tests.py")
        os.exec("env PYTHONPATH=" .. builddir .. ":python " .. python .. " tests/package_pcells_test.py")
        os.exec(python .. " tests/procedural_composition_test.py")
        os.exec(python .. " tests/procedural_router_test.py")
    end)
    set_menu {usage = "Run MCP and Python integration tests"}

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

task("openroad-sdk")
    on_run(function ()
        os.exec("bash scripts/build-ice-openroad.sh")
    end)
    set_menu {usage = "Build the pinned C++ OpenROAD backend"}

-- ---------------------------------------------------------------------------
-- Release packaging
-- ---------------------------------------------------------------------------

local xpack_targets = {"ice-cli", "ice-mcp", "ice-pkg"}
if build_gui then
    table.insert(xpack_targets, 1, "ice")
end

local xpack_formats
if is_windows then
    xpack_formats = {"nsis", "zip"}
elseif is_macos then
    xpack_formats = {"zip"}
else
    xpack_formats = {"targz", "zip"}
end

xpack("ice")
    set_formats(xpack_formats)
    set_title("ICE - Integrated Circuits Electronics Design Automation")
    set_description("Schematic capture, IC layout, analog simulation, analysis, and programmable design workflows.")
    set_homepage("https://github.com/ldapx/ice")
    set_author("ICE contributors")
    set_license("MIT")
    set_licensefile("LICENSE")
    set_basename("ice-v$(version)-$(plat)-$(arch)")
    set_iconfile("assets/ice.ico")
    add_targets(xpack_targets)
    add_installfiles("symbols/*.svg", {prefixdir = "share/ice/symbols"})
    add_installfiles("symbols/*.toml", {prefixdir = "share/ice/symbols"})
    add_installfiles("assets/icons/(**)", {prefixdir = "share/ice/assets/icons"})
    add_installfiles("assets/layout/(*.py)", {prefixdir = "share/ice/assets/layout"})
    add_installfiles("assets/layout/generators/(*.py)", {prefixdir = "share/ice/assets/layout/generators"})
    add_installfiles("assets/simulation/(*.py)", {prefixdir = "share/ice/assets/simulation"})
    add_installfiles("assets/logo.png", {prefixdir = "share/ice/assets"})
    add_installfiles("python/*.py", {prefixdir = "share/ice/python"})
    add_installfiles("README.md", {prefixdir = "share/doc/ice"})
    add_installfiles("LICENSE", {prefixdir = "share/doc/ice"})
    if is_windows then
        -- Bundle the MSYS2/vcpkg runtime DLLs and Qt plugins next to the
        -- executables. The script also copies ngspice code models.
        after_installcmd(function (package, batchcmds)
            batchcmds:runv("bash", {path.join(os.projectdir(), "scripts", "bundle-windows-runtime.sh"),
                                    package:installdir()})
        end)
    elseif is_macos then
        after_installcmd(function (package, batchcmds)
            batchcmds:runv("bash", {path.join(os.projectdir(), "scripts", "bundle-macos-runtime.sh"),
                                    package:installdir()})
        end)
    end
