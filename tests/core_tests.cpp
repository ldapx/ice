#include "icelib/core.hpp"
#include "icelib/geometry.hpp"
#include "icelib/implementation.hpp"
#include "icelib/interchange.hpp"
#include "icelib/layout_library.hpp"
#include "icelib/measure.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "icelib/plugins.hpp"
#include "icelib/simulation.hpp"
#include "icelib/simulation_report.hpp"
#include "icelib/simulation_store.hpp"
#include "icelib/symbols.hpp"
#include "icelib/verification.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
long double polygonArea(const ice::Polygon &polygon) {
    long double area = 0.0L;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto &current = polygon[index];
        const auto &next = polygon[(index + 1) % polygon.size()];
        area += static_cast<long double>(current.x) * next.y - static_cast<long double>(next.x) * current.y;
    }
    return area / 2.0L;
}
} // namespace

TEST_CASE("layout layers preserve GDS datatypes") {
    ice::Project project;
    project.identity = "layer-datatype";
    project.name = "layer-datatype";
    const auto layer = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2, {}, 140);
    project.layers.at(layer).color = "#123456";
    project.layers.at(layer).pattern = 3;
    CHECK(project.layers.at(layer).number == 8);
    CHECK(project.layers.at(layer).datatype == 2);
    CHECK(project.layers.at(layer).minimumWidth == 140);
    CHECK_THROWS_AS(ice::addLayer(project, "bad", 1, 0, {}, 70000), ice::ProjectError);
    CHECK_THROWS_AS(ice::addLayer(project, "bad-width", 1, 0, {}, 0, {}, -1), ice::ProjectError);

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.layers.at(layer).number == 8);
    CHECK(roundTrip.layers.at(layer).datatype == 2);
    CHECK(roundTrip.layers.at(layer).name == "Metal1.pin");
    CHECK(roundTrip.layers.at(layer).color == "#123456");
    CHECK(roundTrip.layers.at(layer).pattern == 3);
    CHECK(roundTrip.layers.at(layer).minimumWidth == 140);

    auto missingDatatype = project.serialize();
    const auto datatype = missingDatatype.find("\"datatype\": 2");
    CHECK(datatype != std::string::npos);
    if (datatype == std::string::npos)
        return;
    const auto lineBegin = missingDatatype.rfind('\n', datatype);
    const auto lineEnd = missingDatatype.find('\n', datatype);
    CHECK(lineBegin != std::string::npos);
    CHECK(lineEnd != std::string::npos);
    if (lineBegin == std::string::npos || lineEnd == std::string::npos)
        return;
    missingDatatype.erase(lineBegin, lineEnd - lineBegin + 1);
    CHECK_THROWS_AS(ice::Project::deserialize(missingDatatype), ice::ProjectError);
}

TEST_CASE("layout cells preserve PDK stack selections") {
    ice::Project project;
    project.identity = "layout-stack";
    project.name = "layout-stack";
    const auto layer = ice::addLayer(project, "Metal1", 34, 0, "gf180mcuA", 0, "3LM_1TM");
    const auto cell = ice::createLayoutCell(project, "top", "gf180mcuA", "3LM_1TM");
    const auto procedural = ice::createProceduralLayout(project, "scripted", "gf180mcuA", "layout = None\n", "6LM_1TM");
    const auto child = ice::createLayoutCell(project, "child", "gf180mcuA", "6LM_1TM");
    CHECK_THROWS_AS(ice::addLayoutInstance(project, cell, child, "I1", 0, 0), ice::ProjectError);
    CHECK(project.layoutCells.at(cell).stack == "3LM_1TM");
    CHECK(project.layoutCells.at(project.proceduralLayouts.at(procedural).generatedCell).stack == "6LM_1TM");
    CHECK(project.layers.at(layer).stack == "3LM_1TM");

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.layoutCells.at(cell).stack == "3LM_1TM");
    CHECK(roundTrip.layoutCells.at(project.proceduralLayouts.at(procedural).generatedCell).stack == "6LM_1TM");
    CHECK(roundTrip.proceduralLayouts.at(procedural).stack == "6LM_1TM");
    CHECK(roundTrip.layers.at(layer).stack == "3LM_1TM");
    CHECK(roundTrip.serialize() == project.serialize());
}

TEST_CASE("implementation runs serialize and persist with the project") {
    const auto root = std::filesystem::temp_directory_path() / "ice-implementation-run-test";
    std::filesystem::remove_all(root);

    ice::Project project;
    project.identity = "implementation-run";
    project.name = "implementation-run";
    const auto sourceId = project.nextId();
    ice::SchematicCell source;
    source.id = sourceId;
    source.name = "counter";
    source.pdk = "example.pdk";
    source.systemVerilogSchematic = true;
    source.systemVerilogText = "module counter(input clk); endmodule\n";
    project.schematicCells.emplace(sourceId, source);

    ice::ImplementationConfig config;
    config.name = "counter-gf180";
    config.top = "counter";
    config.pdk = "example.pdk";
    config.stack = "base";
    config.sourceCells = {sourceId};
    config.utilization = 0.45;
    const auto runId = ice::createImplementationRun(project, config);
    CHECK(runId != 0);
    CHECK(project.implementationRuns.at(runId).name == "counter-gf180");
    CHECK(project.implementationRuns.at(runId).stages.size() == ice::implementationStages().size());
    CHECK(!project.implementationRuns.at(runId).configHash.empty());
    CHECK_THROWS_AS(ice::createImplementationRun(project, config), ice::ProjectError);

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.implementationRuns.at(runId).name == "counter-gf180");
    CHECK(roundTrip.implementationRuns.at(runId).config.top == "counter");
    CHECK(std::abs(roundTrip.implementationRuns.at(runId).config.utilization - 0.45) < 1e-9);
    CHECK(roundTrip.implementationRuns.at(runId).stages.at(ice::ImplementationStage::placement).status ==
          ice::ImplementationStageStatus::pending);

    project.save(root);
    const auto files = std::filesystem::directory_iterator(root / "impl");
    bool foundManifest = false;
    for (const auto &entry : files) {
        if (entry.path().extension() == ".iceimpl") {
            foundManifest = true;
            break;
        }
    }
    CHECK(foundManifest);
    const auto loaded = ice::Project::load(root);
    CHECK(loaded.implementationRuns.at(runId).name == "counter-gf180");
    CHECK(loaded.implementationRuns.at(runId).config.sourceCells.front() == sourceId);

    const auto serialized = ice::serializeImplementationRun(project.implementationRuns.at(runId));
    const auto parsed = ice::deserializeImplementationRun(serialized);
    CHECK(parsed.name == "counter-gf180");
    CHECK(parsed.config.top == "counter");
    CHECK(parsed.status == ice::ImplementationStatus::pending);

    ice::removeImplementationRun(project, runId);
    CHECK(!project.implementationRuns.contains(runId));
    std::filesystem::remove_all(root);
}

TEST_CASE("implementation preparation generates backend scripts") {
    const auto root = std::filesystem::temp_directory_path() / "ice-implementation-prepare-test";
    std::filesystem::remove_all(root);
    const auto packageRoot = root / "package";
    std::filesystem::create_directories(packageRoot / "implementation");
    for (const auto &name : {"tech.lef", "cells.lef", "tt.lib", "example.gds"})
        std::ofstream(packageRoot / "implementation" / name) << "# test data\n";
    std::ofstream(packageRoot / "implementation" / "tapcell.tcl") << "# tapcell\n";
    std::ofstream(packageRoot / "implementation" / "pdn.tcl") << "# pdn\n";

    ice::PackageSpec package;
    package.name = "example.pdk";
    package.version = "1.0.0";
    package.root = packageRoot;
    ice::PackagePdk pdk;
    pdk.id = "example.pdk";
    pdk.defaultStack = "base";
    pdk.implementation = ice::PackagePdkImplementation{};
    pdk.implementation->engine = "openroad";
    pdk.implementation->synthesis = "yosys";
    ice::PackagePdkStack stack;
    stack.id = "base";
    stack.implementation = ice::PackagePdkStackImplementation{
        "example_site",
        "implementation/tech.lef",
        "implementation/cells.lef",
        "implementation/example.gds",
        {{"tt", {"implementation/tt.lib"}}},
        {"Metal2"},
        {"Metal3"},
        "Metal1",
        "Metal3",
        "implementation/tapcell.tcl",
        "implementation/pdn.tcl",
        {},
        {"FILL1"},
    };
    pdk.stacks.push_back(stack);
    package.pdk = pdk;

    ice::Project project;
    project.identity = "implementation-prepare";
    project.name = "implementation-prepare";
    const auto sourceId = project.nextId();
    ice::SchematicCell source;
    source.id = sourceId;
    source.name = "counter";
    source.pdk = "example.pdk";
    source.systemVerilogSchematic = true;
    source.systemVerilogText = "module counter(input clk); endmodule\n";
    project.schematicCells.emplace(sourceId, source);

    ice::ImplementationConfig config;
    config.name = "counter";
    config.top = "counter";
    config.pdk = "example.pdk";
    config.stack = "base";
    config.sourceCells = {sourceId};
    config.ctsRootBuffer = "BUF_X4";
    config.ctsBuffers = {"BUF_X1", "BUF_X2"};
    config.utilization = 0.45;
    const auto runId = ice::createImplementationRun(project, config);
    auto &run = project.implementationRuns.at(runId);

    std::vector<ice::Diagnostic> diagnostics;
    const auto platform = ice::resolveImplementationPlatform(package, "base", diagnostics);
    CHECK(diagnostics.empty());
    CHECK(platform.site == "example_site");
    CHECK(platform.liberty.at("tt").front() == packageRoot / "implementation" / "tt.lib");

    const auto preparation = ice::prepareImplementationRun(project, run, platform, root);
    CHECK(preparation.ready);
    CHECK(std::filesystem::is_regular_file(preparation.runDirectory / "scripts" / "00-synth.ys"));
    CHECK(std::filesystem::is_regular_file(preparation.runDirectory / "scripts" / "01-floorplan.tcl"));
    CHECK(std::filesystem::is_regular_file(preparation.runDirectory / "scripts" / "04-route.tcl"));
    CHECK(std::filesystem::is_regular_file(preparation.runDirectory / "scripts" / "05-final.tcl"));
    std::ifstream floorplan(preparation.runDirectory / "scripts" / "01-floorplan.tcl");
    const std::string floorplanText((std::istreambuf_iterator<char>(floorplan)), {});
    CHECK(floorplanText.find("initialize_floorplan") != std::string::npos);
    CHECK(floorplanText.find("-utilization 45") != std::string::npos);
    CHECK(floorplanText.find("place_pins") != std::string::npos);
    CHECK(floorplanText.find("source") != std::string::npos);
    std::ifstream route(preparation.runDirectory / "scripts" / "04-route.tcl");
    const std::string routeText((std::istreambuf_iterator<char>(route)), {});
    CHECK(routeText.find("global_route") != std::string::npos);
    CHECK(routeText.find("detailed_route") != std::string::npos);
    CHECK(routeText.find("4_cts.odb") != std::string::npos);
    CHECK(routeText.find("5_route.sdc") != std::string::npos);
    std::ifstream cts(preparation.runDirectory / "scripts" / "03-cts.tcl");
    const std::string ctsText((std::istreambuf_iterator<char>(cts)), {});
    CHECK(ctsText.find("clock_tree_synthesis") != std::string::npos);
    CHECK(ctsText.find("BUF_X4") != std::string::npos);

    run.config.ctsEnabled = false;
    const auto withoutCts = ice::prepareImplementationRun(project, run, platform, root);
    CHECK(withoutCts.ready);
    std::ifstream routeWithoutCts(withoutCts.runDirectory / "scripts" / "04-route.tcl");
    const std::string routeWithoutCtsText((std::istreambuf_iterator<char>(routeWithoutCts)), {});
    CHECK(routeWithoutCtsText.find("3_place.odb") != std::string::npos);
    CHECK(routeWithoutCtsText.find("4_cts.odb") == std::string::npos);
    std::ifstream finalStage(withoutCts.runDirectory / "scripts" / "05-final.tcl");
    const std::string finalText((std::istreambuf_iterator<char>(finalStage)), {});
    CHECK(finalText.find("5_route.sdc") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("process runner captures output, timeouts, and cancellation") {
    const auto root = std::filesystem::temp_directory_path() / "ice-process-runner-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    ice::ProcessOptions options;
    options.arguments = {"/bin/sh", "-c", "printf 'hello\\n'; printf 'oops\\n' >&2; exit 7"};
    options.logPath = root / "tool.log";
    options.logTailBytes = 16;
    const auto failed = ice::runProcess(options);
    CHECK(failed.started);
    CHECK(!failed.succeeded());
    CHECK(failed.exitCode == 7);
    CHECK(failed.signal == 0);
    CHECK(failed.logTail.size() <= 16);
    CHECK(failed.logTail.find("oops") != std::string::npos);
    std::ifstream log(options.logPath);
    const std::string logText((std::istreambuf_iterator<char>(log)), {});
    CHECK(logText.find("hello") != std::string::npos);
    CHECK(logText.find("oops") != std::string::npos);

    ice::ProcessOptions timeout;
    timeout.arguments = {"/bin/sh", "-c", "sleep 5"};
    timeout.timeoutSeconds = 1;
    timeout.logPath = root / "timeout.log";
    const auto timedOut = ice::runProcess(timeout);
    CHECK(timedOut.started);
    CHECK(timedOut.timedOut);
    CHECK(!timedOut.succeeded());

    ice::ProcessCancellation cancellation;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancellation.cancel();
    });
    ice::ProcessOptions cancellable;
    cancellable.arguments = {"/bin/sh", "-c", "sleep 5"};
    cancellable.logPath = root / "cancel.log";
    const auto cancelled = ice::runProcess(cancellable, &cancellation);
    canceller.join();
    CHECK(cancelled.started);
    CHECK(cancelled.cancelled);
    CHECK(!cancelled.timedOut);
    CHECK(!cancelled.succeeded());

    std::filesystem::remove_all(root);
}

namespace {

class ScopedEnvironment {
  public:
    ScopedEnvironment(const char *name, const std::string &value) : name_(name) {
        if (const auto *previous = std::getenv(name)) {
            previous_ = previous;
            hadPrevious_ = true;
        }
        setenv(name, value.c_str(), 1);
    }
    ~ScopedEnvironment() {
        if (hadPrevious_)
            setenv(name_.c_str(), previous_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

  private:
    std::string name_;
    std::string previous_;
    bool hadPrevious_ = false;
};

struct TclBackendForTests {
    TclBackendForTests() { setenv("ICE_OPENROAD_BACKEND", "tcl", 1); }
} tclBackendForTests;

void writeExecutable(const std::filesystem::path &path, const std::string &text) {
    std::ofstream(path) << text;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                           std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                           std::filesystem::perms::others_exec);
}

ice::PackagePdkStackImplementation executableTestPlatformFiles() {
    return {"unit",
            "implementation/tech.lef",
            "implementation/cells.lef",
            "implementation/example.gds",
            {{"tt", {"implementation/tt.lib"}}},
            {"Metal2"},
            {"Metal3"},
            "Metal1",
            "Metal3",
            {},
            {},
            {},
            {}};
}

} // namespace

TEST_CASE("implementation execution runs stages and records metrics") {
    const auto root = std::filesystem::temp_directory_path() / "ice-implementation-execute-test";
    std::filesystem::remove_all(root);
    const auto packageRoot = root / "package";
    const auto binRoot = root / "bin";
    std::filesystem::create_directories(packageRoot / "implementation");
    std::filesystem::create_directories(binRoot);
    for (const auto &name : {"tech.lef", "cells.lef", "tt.lib", "example.gds"})
        std::ofstream(packageRoot / "implementation" / name) << "# test data\n";

    writeExecutable(binRoot / "yosys", R"(#!/bin/sh
if [ "$1" = "-V" ]; then
  echo "Fake Yosys 0.99"
  exit 0
fi
mkdir -p synth reports
printf 'module counter(input clk); endmodule\n' > synth/1_synth.v
printf 'module counter(input clk); endmodule\n' > synth/2_tech_map.v
if [ "$1" = "-s" ]; then
  case "$2" in
    *00-synth.ys)
      printf 'digraph G { a -> b }\n' > reports/synthesis-overview.dot
      printf 'digraph G { a -> b }\n' > reports/synthesis.dot
      ;;
    *00-elaborate.ys)
      printf 'digraph G { a -> b }\n' > reports/elaboration.dot
      ;;
  esac
fi
cat <<'STAT'
 Number of cells:                 12
 Number of wires:                 34
 Chip area for module '\counter':  56.75
STAT
exit 0
)");
    writeExecutable(binRoot / "dot", R"(#!/bin/sh
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    *) shift ;;
  esac
done
printf '<svg xmlns="http://www.w3.org/2000/svg"></svg>\n' > "$out"
exit 0
)");
    writeExecutable(binRoot / "openroad", R"(#!/bin/sh
if [ "$1" = "-version" ]; then
  echo "Fake OpenROAD 9.99"
  exit 0
fi
metrics=""
script=""
while [ $# -gt 0 ]; do
  case "$1" in
    -metrics) metrics="$2"; shift 2 ;;
    -threads) shift 2 ;;
    -no_init|-no_splash|-exit) shift ;;
    *) script="$1"; shift ;;
  esac
done
name=$(basename "$script")
if [ -n "$FAKE_OPENROAD_FAIL_STAGE" ] && [ "$name" = "$FAKE_OPENROAD_FAIL_STAGE" ]; then
  echo "fake openroad failure" >&2
  exit 3
fi
mkdir -p db def final reports
case "$name" in
  01-floorplan.tcl) touch db/2_floorplan.odb def/2_floorplan.def final/2_floorplan.sdc ;;
  02-place.tcl) touch db/3_place.odb def/3_place.def final/3_place.sdc ;;
  03-cts.tcl) touch db/4_cts.odb def/4_cts.def final/4_cts.sdc ;;
  04-route.tcl) touch db/5_route.odb def/5_route.def final/5_route.sdc final/5_route.spef reports/route.guide reports/route_drc.rpt ;;
  05-final.tcl) touch db/6_final.odb def/6_final.def final/6_final.v final/6_final.spef final/6_final.sdc ;;
esac
if [ -n "$metrics" ]; then
  printf '{"design__die__area": 1234.5, "route__wirelength": 42}\n' > "$metrics"
fi
echo "fake openroad $name"
exit 0
)");
    ScopedEnvironment yosys("ICE_YOSYS", (binRoot / "yosys").string());
    ScopedEnvironment openroad("ICE_OPENROAD", (binRoot / "openroad").string());
    ScopedEnvironment graphviz("ICE_DOT", (binRoot / "dot").string());

    ice::PackageSpec package;
    package.name = "example.pdk";
    package.version = "1.0.0";
    package.root = packageRoot;
    ice::PackagePdk pdk;
    pdk.id = "example.pdk";
    pdk.defaultStack = "base";
    pdk.implementation = ice::PackagePdkImplementation{};
    ice::PackagePdkStack stack;
    stack.id = "base";
    stack.implementation = executableTestPlatformFiles();
    pdk.stacks.push_back(stack);
    package.pdk = pdk;

    ice::Project project;
    project.identity = "implementation-execute";
    project.name = "implementation-execute";
    const auto sourceId = project.nextId();
    ice::SchematicCell source;
    source.id = sourceId;
    source.name = "counter";
    source.pdk = "example.pdk";
    source.systemVerilogSchematic = true;
    source.systemVerilogText = "module counter(input clk); endmodule\n";
    project.schematicCells.emplace(sourceId, source);

    ice::ImplementationConfig config;
    config.name = "counter";
    config.top = "counter";
    config.pdk = "example.pdk";
    config.stack = "base";
    config.sourceCells = {sourceId};
    config.threads = 2;
    const auto runId = ice::createImplementationRun(project, config);
    auto &run = project.implementationRuns.at(runId);
    std::vector<ice::Diagnostic> diagnostics;
    const auto platform = ice::resolveImplementationPlatform(package, "base", diagnostics);
    CHECK(diagnostics.empty());

    std::vector<ice::ImplementationProgress> progress;
    std::size_t changes = 0;
    ice::ImplementationExecutionOptions options;
    options.progress = [&](const ice::ImplementationProgress &event) { progress.push_back(event); };
    options.changed = [&](const ice::ImplementationRun &) { ++changes; };
    const auto result = ice::executeImplementationRun(project, run, platform, root, options);
    CHECK(result.success);
    CHECK(!result.cancelled);
    CHECK(result.completedStage == ice::ImplementationStage::final);
    CHECK(run.status == ice::ImplementationStatus::succeeded);
    for (const auto stage :
         {ice::ImplementationStage::synthesis, ice::ImplementationStage::floorplan, ice::ImplementationStage::placement,
          ice::ImplementationStage::cts, ice::ImplementationStage::routing, ice::ImplementationStage::final})
        CHECK(run.stages.at(stage).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.toolVersions.at("yosys") == "Fake Yosys 0.99");
    CHECK(run.toolVersions.at("openroad") == "Fake OpenROAD 9.99");
    CHECK(std::abs(run.metrics.at("synth.cells") - 12.0) < 1e-9);
    CHECK(std::abs(run.metrics.at("synth.chip_area_um2") - 56.75) < 1e-9);
    CHECK(std::abs(run.metrics.at("design__die__area") - 1234.5) < 1e-9);
    CHECK(std::abs(run.metrics.at("route__wirelength") - 42.0) < 1e-9);
    CHECK(changes > 0);
    CHECK(!progress.empty());
    CHECK(std::filesystem::is_regular_file(root / run.directory / "db" / "6_final.odb"));
    CHECK(std::filesystem::is_regular_file(root / run.directory / "final" / "6_final.v"));
    CHECK(std::filesystem::is_regular_file(root / run.directory / "logs" / "05-final.log"));
    CHECK(std::filesystem::is_regular_file(root / run.directory / "reports" / "route_drc.rpt"));
    CHECK(std::any_of(run.stages.at(ice::ImplementationStage::routing).artifacts.begin(),
                      run.stages.at(ice::ImplementationStage::routing).artifacts.end(),
                      [](const std::string &artifact) { return artifact.ends_with("reports/route_drc.rpt"); }));
    CHECK(std::filesystem::is_regular_file(root / run.directory / "reports" / "synthesis.svg"));
    CHECK(!std::filesystem::is_regular_file(root / run.directory / "reports" / "synthesis.dot"));
    CHECK(!std::filesystem::is_regular_file(root / run.directory / "reports" / "synthesis-overview.dot"));
    CHECK(ice::readImplementationLog(run, root, ice::ImplementationStage::final).find("fake openroad") !=
          std::string::npos);

    // Rerunning one stage invalidates only that stage and later ones.
    ice::ImplementationExecutionOptions placementOnly;
    placementOnly.from = ice::ImplementationStage::placement;
    placementOnly.to = ice::ImplementationStage::placement;
    const auto rerun = ice::executeImplementationRun(project, run, platform, root, placementOnly);
    CHECK(rerun.success);
    CHECK(run.status == ice::ImplementationStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::synthesis).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::floorplan).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::placement).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::routing).status == ice::ImplementationStageStatus::pending);
    CHECK(run.stages.at(ice::ImplementationStage::final).status == ice::ImplementationStageStatus::pending);

    // A failing stage preserves the earlier checkpoint and records the log.
    setenv("FAKE_OPENROAD_FAIL_STAGE", "04-route.tcl", 1);
    ice::ImplementationExecutionOptions failure;
    failure.from = ice::ImplementationStage::routing;
    failure.to = ice::ImplementationStage::final;
    const auto failed = ice::executeImplementationRun(project, run, platform, root, failure);
    unsetenv("FAKE_OPENROAD_FAIL_STAGE");
    CHECK(!failed.success);
    CHECK(run.status == ice::ImplementationStatus::failed);
    CHECK(run.stages.at(ice::ImplementationStage::placement).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::routing).status == ice::ImplementationStageStatus::failed);
    CHECK(run.stages.at(ice::ImplementationStage::final).status == ice::ImplementationStageStatus::pending);
    CHECK(run.lastError.find("04-route") != std::string::npos ||
          run.lastError.find("Routing failed") != std::string::npos);

    // Exporting maps the generic netlist and records the mapped artifact.
    const auto mapped = ice::mapImplementationRun(run, platform, root, root / "export" / "counter.v");
    CHECK(mapped.success);
    CHECK(std::filesystem::is_regular_file(root / run.directory / "synth" / "2_tech_map.v"));
    CHECK(std::filesystem::is_regular_file(root / "export" / "counter.v"));
    const auto &synthArtifacts = run.stages.at(ice::ImplementationStage::synthesis).artifacts;
    const auto mappedArtifact = run.directory + "/synth/2_tech_map.v";
    CHECK(std::find(synthArtifacts.begin(), synthArtifacts.end(), mappedArtifact) != synthArtifacts.end());
    std::ifstream techmapScript(root / run.directory / "scripts" / "01-techmap.ys");
    const std::string techmapText((std::istreambuf_iterator<char>(techmapScript)), {});
    CHECK(techmapText.find("dfflibmap -liberty") != std::string::npos);
    CHECK(techmapText.find("abc -liberty") != std::string::npos);

    // Editing floorplan policy keeps earlier stages and invalidates floorplan
    // and everything after it without resetting the whole run.
    auto updatedConfig = run.config;
    updatedConfig.utilization = 0.7;
    ice::updateImplementationConfig(run, updatedConfig, ice::ImplementationStage::floorplan);
    CHECK(run.config.utilization == 0.7);
    CHECK(run.configHash == ice::implementationConfigHash(run.config));
    CHECK(run.stages.at(ice::ImplementationStage::synthesis).status == ice::ImplementationStageStatus::succeeded);
    CHECK(run.stages.at(ice::ImplementationStage::floorplan).status == ice::ImplementationStageStatus::pending);
    CHECK(run.stages.at(ice::ImplementationStage::placement).status == ice::ImplementationStageStatus::pending);
    CHECK(run.currentStage == ice::ImplementationStage::synthesis);
    CHECK(run.status == ice::ImplementationStatus::pending);

    std::filesystem::remove_all(root);
}

TEST_CASE("DEF import creates pins, instances, nets, and routes") {
    const auto root = std::filesystem::temp_directory_path() / "ice-def-import-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto techLef = root / "tech.lef";
    std::ofstream(techLef) << R"(VERSION 5.8 ;
DATABASE MICRONS 1000 ;
LAYER Metal1
  TYPE ROUTING ;
  WIDTH 0.2 ;
END Metal1
LAYER Metal2
  TYPE ROUTING ;
  WIDTH 0.4 ;
END Metal2
LAYER Via1
  TYPE CUT ;
END Via1
VIA via1 DEFAULT
  LAYER Metal1 ;
    RECT -0.1 -0.1 0.1 0.1 ;
  LAYER Via1 ;
    RECT -0.05 -0.05 0.05 0.05 ;
  LAYER Metal2 ;
    RECT -0.1 -0.1 0.1 0.1 ;
END via1
END LIBRARY
)";
    const auto cellLef = root / "cells.lef";
    std::ofstream(cellLef) << R"(VERSION 5.8 ;
MACRO INV
  CLASS CORE ;
  SIZE 1 BY 2 ;
  PIN A
    DIRECTION INPUT ;
    USE SIGNAL ;
    PORT
      LAYER Metal1 ;
        RECT 0 0 0.2 0.2 ;
    END
  END A
  PIN Y
    DIRECTION OUTPUT ;
    USE SIGNAL ;
    PORT
      LAYER Metal1 ;
        RECT 0.8 0.8 1 1 ;
    END
  END Y
END INV
END LIBRARY
)";
    const auto def = root / "top.def";
    std::ofstream(def) << R"(VERSION 5.8 ;
DESIGN top ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 10000 10000 ) ;
COMPONENTS 2 ;
- u1 INV + PLACED ( 1000 2000 ) N ;
- u2 INV + PLACED ( 3000 2000 ) S ;
END COMPONENTS
PINS 2 ;
- in1 + NET in1 + DIRECTION INPUT + USE SIGNAL + LAYER Metal1 ( -100 -100 ) ( 100 100 ) ;
- out1 + NET out1 + DIRECTION OUTPUT + USE SIGNAL + LAYER Metal1 ( 9900 9900 ) ( 10100 10100 ) ;
END PINS
NETS 3 ;
- in1 ( u1 A ) + ROUTED Metal1 ( 0 0 ) ( 1000 2000 ) via1 ( 3000 2000 ) ;
- out1 ( u2 Y ) + ROUTED Metal2 ( 3000 2000 ) ( 9000 9000 ) ;
- n1 ( u1 Y ) ( u2 A ) + ROUTED Metal1 ( 2000 2000 ) ( 2500 2000 ) NEW Metal2 ( 2500 2000 ) ( 3000 2000 ) ;
END NETS
END DESIGN
)";

    ice::DefImportOptions options;
    options.techLef = techLef;
    options.cellLef = cellLef;
    options.pdk = "example.pdk";
    options.stack = "base";
    options.layers["metal1"] = {34, 0};
    options.layers["metal2"] = {36, 0};
    options.layers["via1"] = {35, 0};

    ice::Project project;
    project.identity = "def-import";
    project.name = "def-import";
    const auto cellId = ice::LayoutInterchange::importDefIntoProject(project, def, options);
    const auto &cell = project.layoutCells.at(cellId);
    CHECK(cell.name == "top");
    CHECK(cell.pdk == "example.pdk");
    CHECK(cell.stack == "base");
    CHECK(cell.ports.size() == 2);
    CHECK(cell.nets.size() == 3);
    CHECK(cell.instances.size() == 2);
    CHECK(cell.shapes.size() == 5); // die boundary + four routed paths

    const auto in1 =
        std::find_if(cell.nets.begin(), cell.nets.end(), [](const auto &entry) { return entry.second.name == "in1"; });
    CHECK(in1 != cell.nets.end());
    const auto port = std::find_if(cell.ports.begin(), cell.ports.end(),
                                   [](const auto &entry) { return entry.second.name == "in1"; });
    CHECK(port != cell.ports.end());
    CHECK(port->second.net == in1->second.id);
    CHECK(port->second.rectangle.width == 200);

    const auto u1 = std::find_if(cell.instances.begin(), cell.instances.end(),
                                 [](const auto &entry) { return entry.second.name == "u1"; });
    CHECK(u1 != cell.instances.end());
    CHECK(u1->second.x == 1000);
    CHECK(u1->second.y == 2000);
    CHECK(u1->second.orientation == ice::Orientation::r0);
    const auto u2 = std::find_if(cell.instances.begin(), cell.instances.end(),
                                 [](const auto &entry) { return entry.second.name == "u2"; });
    CHECK(u2 != cell.instances.end());
    CHECK(u2->second.orientation == ice::Orientation::r180);
    const auto &master = project.layoutCells.at(u1->second.cell);
    CHECK(master.name == "INV");
    CHECK(master.ports.size() == 2);
    CHECK(master.shapes.size() == 1);

    const auto route = std::find_if(cell.shapes.begin(), cell.shapes.end(), [](const auto &entry) {
        return entry.second.net && entry.second.centerline.size() == 3 && !entry.second.pointVias.empty();
    });
    CHECK(route != cell.shapes.end());
    if (route != cell.shapes.end()) {
        CHECK(route->second.width == 200);
        CHECK(route->second.pointVias.size() == route->second.centerline.size());
        CHECK(!route->second.pointVias[1].empty());
    }
    const auto wideRoute = std::find_if(cell.shapes.begin(), cell.shapes.end(),
                                        [](const auto &entry) { return entry.second.width == 400; });
    CHECK(wideRoute != cell.shapes.end());
    CHECK(project.validate().empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("DEF import is numeric-locale independent") {
    struct LocaleGuard {
        std::string previous;
        ~LocaleGuard() {
            if (!previous.empty())
                std::setlocale(LC_NUMERIC, previous.c_str());
        }
    } localeGuard;
    if (const auto *current = std::setlocale(LC_NUMERIC, nullptr))
        localeGuard.previous = current;
    // Qt installs the user's numeric locale; decimal commas must not break
    // LEF/DEF token parsing.
    if (!std::setlocale(LC_NUMERIC, "es_CL.UTF-8"))
        return;

    const auto root = std::filesystem::temp_directory_path() / "ice-def-locale-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto techLef = root / "tech.lef";
    std::ofstream(techLef) << "VERSION 5.8 ;\nDATABASE MICRONS 1000 ;\nLAYER Metal1\n"
                              "  TYPE ROUTING ;\n  WIDTH 0.2 ;\nEND Metal1\nEND LIBRARY\n";
    const auto cellLef = root / "cells.lef";
    std::ofstream(cellLef) << "VERSION 5.8 ;\nMACRO INV\n  CLASS CORE ;\n  SIZE 0.500 BY 1.200 ;\n"
                              "  PIN A\n    DIRECTION INPUT ;\n    PORT\n      LAYER Metal1 ;\n"
                              "        RECT 0.100 0.100 0.230 0.230 ;\n    END\n  END A\nEND INV\nEND LIBRARY\n";
    const auto def = root / "top.def";
    std::ofstream(def) << "VERSION 5.8 ;\nDESIGN top ;\nUNITS DISTANCE MICRONS 1000 ;\n"
                          "DIEAREA ( 0 0 ) ( 10500 10500 ) ;\nCOMPONENTS 1 ;\n"
                          "- u1 INV + PLACED ( 230 345 ) N ;\nEND COMPONENTS\nEND DESIGN\n";

    ice::DefImportOptions options;
    options.techLef = techLef;
    options.cellLef = cellLef;
    options.pdk = "example.pdk";
    options.stack = "base";
    options.layers["metal1"] = {34, 0};

    ice::Project project;
    project.identity = "def-locale";
    project.name = "def-locale";
    const auto cellId = ice::LayoutInterchange::importDefIntoProject(project, def, options);
    CHECK(cellId != 0);
    const auto &cell = project.layoutCells.at(cellId);
    CHECK(cell.instances.size() == 1);
    CHECK(cell.instances.begin()->second.x == 230);
    CHECK(cell.instances.begin()->second.y == 345);

    std::filesystem::remove_all(root);
}

TEST_CASE("text polygon shape groups survive serialization") {
    ice::Project project;
    project.identity = "text-polygon";
    project.name = "text-polygon";
    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto cell = ice::createLayoutCell(project, "top");
    const auto first = ice::addPolygon(project, cell, layer, {{0, 0}, {100, 0}, {100, 100}, {0, 100}});
    const auto second = ice::addPolygon(project, cell, layer, {{100, 0}, {200, 0}, {200, 100}, {100, 100}});
    auto &cellData = project.layoutCells.at(cell);
    for (const auto id : {first, second}) {
        auto &shape = cellData.shapes.at(id);
        shape.name = "Text Polygon 1";
        shape.textPolygon = true;
        shape.textGroup = first;
        shape.textSource = "AB";
    }

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    const auto &restored = roundTrip.layoutCells.at(cell).shapes;
    CHECK(restored.at(first).textPolygon);
    CHECK(restored.at(first).textGroup == first);
    CHECK(restored.at(first).textSource == "AB");
    CHECK(!restored.at(first).isText());
    CHECK(restored.at(second).textGroup == first);
}

TEST_CASE("project serialization and transactions are deterministic") {
    ice::Project project;
    project.identity = "demo";
    project.name = "demo";
    project.packageDependencies = {"example.pdk"};
    auto layerId = project.nextId();
    project.layers.emplace(layerId, ice::Layer{layerId, "metal1", 1});
    auto schematicId = project.nextId();
    ice::SchematicCell schematic;
    schematic.id = schematicId;
    schematic.name = "top";
    schematic.pdk = "example.pdk";
    auto symbolId = project.nextId();
    ice::Symbol symbol;
    symbol.id = symbolId;
    symbol.name = "resistor";
    symbol.label = ice::SymbolLabel{3, 9};
    auto pinId = project.nextId();
    symbol.pins.push_back({pinId, "A", 0, 0});
    schematic.symbols.emplace(symbolId, symbol);
    auto instanceId = project.nextId();
    schematic.instances.emplace(instanceId, ice::SchematicInstance{instanceId, symbolId, "R1", 0, 0});
    auto netId = project.nextId();
    schematic.nets.emplace(netId, ice::Net{netId, "net1", {pinId}});
    project.schematicCells.emplace(schematicId, schematic);
    auto cellId = project.nextId();
    ice::LayoutCell cell;
    cell.id = cellId;
    cell.name = "top";
    cell.pdk = "example.pdk";
    auto shapeId = project.nextId();
    cell.shapes.emplace(shapeId, ice::Shape{shapeId, layerId, std::nullopt, {0, 0, 100, 50}});
    project.layoutCells.emplace(cellId, cell);
    CHECK(project.validate().empty());
    auto serialized = project.serialize();
    auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label.has_value());
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label->x == 3);
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label->y == 9);
    CHECK(roundTrip.layoutCells.at(cellId).pdk == "example.pdk");

    auto invalid = project;
    invalid.layoutCells.at(cellId).shapes.at(shapeId).layer = 9999;
    CHECK(!invalid.validate().empty());

    ice::ProjectSession session(roundTrip);
    bool notified = false;
    session.subscribe([&] { notified = true; });
    session.execute("rename", [](ice::Project &value) { value.name = "changed"; });
    CHECK(session.dirty());
    CHECK(notified);
    CHECK(session.project().name == "changed");
    CHECK(session.undo());
    CHECK(session.project().name == "demo");
    CHECK(session.redo());
    CHECK(session.project().name == "changed");
    CHECK_THROWS_AS(session.execute("invalid", [](ice::Project &value) { value.grid = 0; }), ice::ProjectError);
    CHECK(session.project().grid == 1);
}

TEST_CASE("geometry operations are deterministic and queryable") {
    ice::GeometryKernel kernel;
    ice::MultiPolygon first = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}};
    ice::MultiPolygon second = {{{5, 0}, {15, 0}, {15, 10}, {5, 10}}};
    auto intersection = kernel.booleanOp(first, second, ice::BooleanOperation::intersection);
    CHECK(intersection.size() == 1);
    CHECK(ice::GeometryKernel::bounds(intersection.front()).width == 5);
    CHECK(polygonArea(intersection.front()) == 50.0L);
    auto united = kernel.booleanOp(first, second, ice::BooleanOperation::union_);
    CHECK(united.size() == 1);
    CHECK(polygonArea(united.front()) == 150.0L);
    auto difference = kernel.booleanOp(first, second, ice::BooleanOperation::difference);
    CHECK(difference.size() == 1);
    CHECK(polygonArea(difference.front()) == 50.0L);
    auto exclusive = kernel.booleanOp(first, second, ice::BooleanOperation::xor_);
    CHECK(exclusive.size() == 2);
    long double exclusiveArea = 0.0L;
    for (const auto &polygon : exclusive)
        exclusiveArea += std::abs(polygonArea(polygon));
    CHECK(exclusiveArea == 100.0L);
    auto expanded = kernel.offset(first, 2);
    CHECK(!expanded.empty());

    ice::SpatialIndex index;
    index.insert(10, {0, 0, 10, 10});
    index.insert(20, {20, 20, 5, 5});
    CHECK(index.query({5, 5, 2, 2}) == std::vector<ice::ObjectId>{10});
    CHECK(index.query({10, 10, 2, 2}).empty());

    ice::ArraySpec array{2, 1, 20, 0};
    auto transformed = ice::transformArray(first, ice::Transform{100, 50, ice::Orientation::r0}, array);
    CHECK(transformed.size() == 2);
    CHECK(transformed[1].front().x == 120);
}

TEST_CASE("selection, cross-probing, and grid snapping are stable") {
    ice::Project project;
    project.identity = "selection";
    project.name = "selection";
    project.grid = 10;
    CHECK(project.snap(14) == 10);
    CHECK(project.snap(16) == 20);
    CHECK(project.snap(-14) == -10);
    CHECK(project.snap(-16) == -20);
    CHECK(project.snap(0) == 0);
    CHECK(project.snap(5) == 10);
    CHECK(project.snap(-5) == 0);

    ice::SelectionModel selection;
    selection.select(42, {7, 8});
    CHECK(selection.primary() == std::optional<ice::ObjectId>{42});
    CHECK((selection.related() == std::vector<ice::ObjectId>{7, 8}));
    selection.clear();
    CHECK(!selection.primary().has_value());
    CHECK(selection.related().empty());
}

TEST_CASE("projects save as split directory files") {
    const auto root = std::filesystem::temp_directory_path() / "ice-split-project-test";
    std::filesystem::remove_all(root);

    ice::Project project;
    project.identity = "split-project";
    project.name = "split-project";
    project.packageDependencies = {"example.pdk"};
    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto schematic = ice::createSchematicCell(project, "amp", "example.pdk");
    const auto textSchematic = ice::createTextSchematicCell(project, "spice_top", "example.pdk", ".title spice_top\n");
    const auto layout = ice::createLayoutCell(project, "top", "example.pdk");
    ice::addRectangle(project, layout, layer, {0, 0, 1000, 500});
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "tran1";
    simulation.schematic = schematic;
    project.simulations.emplace(simulation.id, simulation);
    const auto report = project.nextId();
    project.reports.emplace(report, ice::ProjectReport{report, "compare", "scripts/compare.py"});

    project.save(root);
    CHECK(std::filesystem::is_regular_file(root / "project.icepro"));
    CHECK(std::filesystem::is_directory(root / "sch"));
    CHECK(std::filesystem::is_directory(root / "lay"));
    CHECK(std::filesystem::is_directory(root / "sim"));
    CHECK(std::filesystem::is_directory(root / "scripts"));
    CHECK(std::filesystem::is_directory(root / "build"));
    CHECK(std::filesystem::is_regular_file(root / ".gitignore"));
    CHECK(std::filesystem::is_regular_file(root / "AGENTS.md"));
    std::ifstream agentsStream(root / "AGENTS.md");
    const std::string agentsText((std::istreambuf_iterator<char>(agentsStream)), {});
    CHECK(agentsText.find("typical workflow") != std::string::npos);
    CHECK(agentsText.find("tweak the schematic") != std::string::npos);
    CHECK(agentsText.find("simulation-run") != std::string::npos);
    CHECK(agentsText.find("saved to disk automatically") != std::string::npos);
    CHECK(agentsText.find("scripts/*.py") != std::string::npos);
    CHECK(agentsText.find("simulation script") != std::string::npos);
    CHECK(agentsText.find("canvas schematic") != std::string::npos);
    CHECK(agentsText.find("text SPICE schematic") != std::string::npos);
    CHECK(agentsText.find("showcase the schematic behavior") != std::string::npos);
    CHECK(agentsText.find("useful circuit metrics") != std::string::npos);
    CHECK(agentsText.find("array of simulations") != std::string::npos);
    CHECK(agentsText.find("normal simulation for simplicity") != std::string::npos);
    CHECK(agentsText.find("ice_doctor") != std::string::npos);
    CHECK(agentsText.find("schematic_add_net") != std::string::npos);
    CHECK(agentsText.find("available signals") != std::string::npos);
    CHECK(agentsText.find("Do not guess an absolute PDK") != std::string::npos);
    CHECK(agentsText.find("ask the user before drawing") != std::string::npos);
    CHECK(agentsText.find("schematic specifications") != std::string::npos);
    CHECK(agentsText.find("Never write or edit") != std::string::npos);
    CHECK(agentsText.find("project_repair") != std::string::npos);
    CHECK(agentsText.find("behavioral or RC approximation") != std::string::npos);
    CHECK(agentsText.find("zero crossings") != std::string::npos);

    std::ifstream projectStream(root / "project.icepro");
    const std::string projectJson((std::istreambuf_iterator<char>(projectStream)), {});
    CHECK(projectJson.find("schematic_cells") == std::string::npos);
    CHECK(projectJson.find("layout_cells") == std::string::npos);
    CHECK(projectJson.find("simulations") == std::string::npos);
    CHECK(projectJson.find("\"simulation_scripts\"") != std::string::npos);
    std::ifstream simulationFile(root / "sim" / "tran1.icesim");
    const std::string simulationJson((std::istreambuf_iterator<char>(simulationFile)), {});
    CHECK(simulationJson.find("reports") == std::string::npos);
    CHECK(std::filesystem::is_directory(root / "scripts"));

    const auto loaded = ice::Project::load(root / "project.icepro");
    CHECK(loaded.name == project.name);
    CHECK(loaded.schematicCells.contains(schematic));
    CHECK(loaded.schematicCells.contains(textSchematic));
    CHECK(loaded.layoutCells.contains(layout));
    CHECK(loaded.simulations.contains(simulation.id));
    CHECK(loaded.reports.contains(report));
    CHECK(loaded.serialize() == project.serialize());

    std::ifstream ignoreStream(root / ".gitignore");
    const std::string ignoreText((std::istreambuf_iterator<char>(ignoreStream)), {});
    CHECK(ignoreText.find("reports/") == std::string::npos);
    CHECK(ignoreText.find("scripts/") == std::string::npos);
    CHECK(ignoreText.find("build/") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("schematic instance parameters survive serialization") {
    ice::Project project;
    project.identity = "params";
    project.name = "params";
    const auto cellId = ice::createSchematicCell(project, "top");
    const auto symbolId = ice::addSymbol(project, cellId, "resistor");
    const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, "R1", 0, 0);
    project.schematicCells.at(cellId).instances.at(instanceId).parameters.push_back({project.nextId(), "r", "1k"});
    CHECK(project.validate().empty());
    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    const auto &restored = roundTrip.schematicCells.at(cellId).instances.at(instanceId).parameters;
    CHECK(restored.size() == 1);
    CHECK(restored.front().name == "r");
    CHECK(restored.front().value == "1k");
}

TEST_CASE("hierarchical symbols keep their source schematic") {
    ice::Project project;
    project.identity = "hierarchy";
    project.name = "hierarchy";
    const auto childCellId = ice::createSchematicCell(project, "child");
    const auto parentCellId = ice::createSchematicCell(project, "parent");
    const auto symbolId = ice::addSymbol(project, parentCellId, "child", {}, childCellId);
    const auto instanceId = ice::addSchematicInstance(project, parentCellId, symbolId, "child1", 0, 0);
    CHECK(project.validate().empty());
    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.schematicCells.at(parentCellId).symbols.at(symbolId).sourceCell == childCellId);
    CHECK(roundTrip.schematicCells.at(parentCellId).instances.contains(instanceId));
}

TEST_CASE("schematic and layout names are unique") {
    ice::Project project;
    project.identity = "unique-names";
    project.name = "unique-names";

    const auto schematicId = ice::createSchematicCell(project, "top");
    CHECK_THROWS_AS(ice::createSchematicCell(project, "top"), ice::ProjectError);
    CHECK_THROWS_AS(ice::createTextSchematicCell(project, "top"), ice::ProjectError);
    const auto textId = ice::createTextSchematicCell(project, "spice_top");
    CHECK(textId != 0);
    CHECK_THROWS_AS(ice::createTextSchematicCell(project, "spice_top"), ice::ProjectError);
    CHECK(project.schematicCells.at(schematicId).name == "top");

    const auto layoutId = ice::createLayoutCell(project, "layout_top");
    CHECK_THROWS_AS(ice::createLayoutCell(project, "layout_top"), ice::ProjectError);
    const auto proceduralId = ice::createProceduralLayout(project, "procedural_top");
    CHECK_THROWS_AS(ice::createProceduralLayout(project, "procedural_top"), ice::ProjectError);
    CHECK_THROWS_AS(ice::createLayoutCell(project, "procedural_top"), ice::ProjectError);
    CHECK_THROWS_AS(ice::createProceduralLayout(project, "layout_top"), ice::ProjectError);
    CHECK(project.layoutCells.at(layoutId).name == "layout_top");
    CHECK(project.proceduralLayouts.at(proceduralId).name == "procedural_top");
    CHECK(project.validate().empty());
}

TEST_CASE("text SPICE schematics survive serialization") {
    ice::Project project;
    project.identity = "spice-text";
    project.name = "spice-text";
    const auto cellId =
        ice::createTextSchematicCell(project, "amp", "ihp.sg13g2", "* SPICE schematic: amp\n.title amp\n.end\n");
    const auto &cell = project.schematicCells.at(cellId);
    CHECK(cell.textSchematic);
    CHECK(cell.spiceText.find(".title") != std::string::npos);
    CHECK(project.validate().empty());

    project.schematicCells.at(cellId).spiceText =
        "* text circuit\nV1 in 0 1\nR1 in out 1k\n.tran 1u 1m\n.ac dec 10 1 1meg\n.end\n";
    const auto netlist = ice::buildSimulationNetlist(project, cellId);
    const std::vector<std::string> expectedNetlist = {"* text circuit", "V1 in 0 1", "R1 in out 1k"};
    CHECK(netlist == expectedNetlist);

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.schematicCells.at(cellId).textSchematic);
    CHECK(roundTrip.schematicCells.at(cellId).spiceText == cell.spiceText);
}

TEST_CASE("SystemVerilog schematics survive serialization") {
    ice::Project project;
    project.identity = "systemverilog";
    project.name = "systemverilog";
    const std::string source = "module counter(input logic clk, output logic q);\n"
                               "    always_ff @(posedge clk) q <= ~q;\n"
                               "endmodule\n";
    const auto cellId = ice::createSystemVerilogCell(project, "counter", "ihp.sg13g2", source);
    const auto &cell = project.schematicCells.at(cellId);
    CHECK(cell.systemVerilogSchematic);
    CHECK(!cell.textSchematic);
    CHECK(cell.isTextSource());
    CHECK(cell.sourceText() == source);
    CHECK(project.validate().empty());
    CHECK_THROWS_AS(ice::createSystemVerilogCell(project, "counter"), ice::ProjectError);
    CHECK_THROWS_AS(ice::buildSimulationNetlist(project, cellId), ice::ProjectError);

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.schematicCells.at(cellId).systemVerilogSchematic);
    CHECK(roundTrip.schematicCells.at(cellId).systemVerilogText == source);
    CHECK(roundTrip.schematicCells.at(cellId).sourceText() == source);
}

TEST_CASE("a cell cannot be both SPICE text and SystemVerilog") {
    ice::Project project;
    project.identity = "mixed-source";
    project.name = "mixed-source";
    const auto cellId = ice::createTextSchematicCell(project, "mixed", ice::genericPdkId, ".title mixed\n.end\n");
    project.schematicCells.at(cellId).systemVerilogSchematic = true;
    const auto diagnostics = project.validate();
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
                      [](const ice::Diagnostic &diagnostic) { return diagnostic.code == "schematic.source"; }));
}

TEST_CASE("symbol default parameters are seeded without duplicates") {
    ice::Project project;
    project.identity = "defaults";
    project.name = "defaults";
    const auto cellId = ice::createSchematicCell(project, "top");
    const auto symbolId = ice::addSymbol(project, cellId, "Voltage Source");
    const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, "V1", 0, 0);
    auto &instance = project.schematicCells.at(cellId).instances.at(instanceId);

    const std::vector<ice::Parameter> defaults = {{0, "voltage", "1"}, {0, "gain", "2"}};
    CHECK(ice::seedDefaultParameters(project, instance, defaults) == 2);
    CHECK(instance.parameters.size() == 2);
    CHECK(instance.parameters[0].id != 0);
    CHECK(instance.parameters[1].id != 0);
    CHECK(instance.parameters[0].id != instance.parameters[1].id);
    CHECK(instance.parameters[0].name == "voltage");
    CHECK(instance.parameters[0].value == "1");
    // Re-seeding keeps existing values and does not duplicate names.
    CHECK(ice::seedDefaultParameters(project, instance, defaults) == 0);
    CHECK(instance.parameters.size() == 2);
}

TEST_CASE("simulation graph axis limits are optional and survive serialization") {
    ice::Project project;
    project.identity = "graphs";
    project.name = "graphs";
    const auto simId = project.nextId();
    ice::SimulationItem sim;
    sim.id = simId;
    sim.name = "tran";
    sim.use_initial_conditions = true;
    sim.captureSignals = "v(out), v(in)";
    ice::SimulationGraph automatic;
    automatic.id = project.nextId();
    automatic.name = "auto";
    automatic.ySignals = {"v(out)"};
    ice::SimulationGraph limited;
    limited.id = project.nextId();
    limited.name = "limited";
    limited.xAxis = "v(in)";
    limited.ySignals = {"v(out)", "v(in)"};
    limited.xMin = 1e-9;
    limited.xMax = 2e-9;
    limited.yMin = -0.5;
    limited.yMax = 1.5;
    sim.graphs = {automatic, limited};
    project.simulations.emplace(simId, sim);

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.simulations.at(simId).use_initial_conditions);
    CHECK(roundTrip.simulations.at(simId).captureSignals == "v(out), v(in)");
    const auto &graphs = roundTrip.simulations.at(simId).graphs;
    CHECK(graphs[0].xAxis == "time");
    CHECK((graphs[0].ySignals == std::vector<std::string>{"v(out)"}));
    CHECK(graphs[1].xAxis == "v(in)");
    CHECK((graphs[1].ySignals == std::vector<std::string>{"v(out)", "v(in)"}));
    CHECK(graphs.size() == 2);
    CHECK(!graphs[0].xMin.has_value());
    CHECK(!graphs[0].xMax.has_value());
    CHECK(!graphs[0].yMin.has_value());
    CHECK(!graphs[0].yMax.has_value());
    CHECK(graphs[1].xMin == std::optional<double>{1e-9});
    CHECK(graphs[1].xMax == std::optional<double>{2e-9});
    CHECK(graphs[1].yMin == std::optional<double>{-0.5});
    CHECK(graphs[1].yMax == std::optional<double>{1.5});
    CHECK(roundTrip.serialize() == serialized);
}

TEST_CASE("geometry transforms match Qt rotation convention") {
    const ice::Point point{10, 0};
    CHECK((ice::Transform{0, 0, ice::Orientation::r0}.apply(point) == ice::Point{10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r90}.apply(point) == ice::Point{0, 10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r180}.apply(point) == ice::Point{-10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r270}.apply(point) == ice::Point{0, -10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::m45}.apply(point) == ice::Point{0, 10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::m135}.apply(point) == ice::Point{0, -10}));
}

TEST_CASE("ngspice simulation is numeric-locale independent") {
    struct LocaleGuard {
        std::string previous;
        ~LocaleGuard() {
            if (!previous.empty())
                std::setlocale(LC_NUMERIC, previous.c_str());
        }
    } localeGuard;
    if (const auto *current = std::setlocale(LC_NUMERIC, nullptr))
        localeGuard.previous = current;
    // The GUI installs the user's numeric locale. ngspice parses netlist
    // numbers through strtod(), which would otherwise treat 1.5 as two tokens.
    if (!std::setlocale(LC_NUMERIC, "es_CL.UTF-8"))
        return;

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 0.1e-6;
    request.stop = 2e-6;
    request.netlist = {"* locale RC", "V1 1 0 1", "R1 1 2 1.5k", "C1 2 0 0.5u"};
    request.probeSignals = {"2"};
    ice::SimulationRunner runner;
    const auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty()) {
        CHECK(!result.time.empty());
        CHECK(result.traces.front().values.size() == result.time.size());
        CHECK(*std::max_element(result.traces.front().values.begin(), result.traces.front().values.end()) > 0.5);
    }
}

TEST_CASE("ngspice simulation reports missing netlists explicitly") {
    ice::SimulationRequest request;
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(1));
    CHECK(result.state == ice::JobState::failed);
    CHECK(result.error.find("netlist") != std::string::npos);

    ice::SimulationRequest unsupported = request;
    unsupported.netlist = {"* test", ".end"};
    unsupported.analysis = "ac";
    auto rejected = runner.run(unsupported, std::chrono::seconds(1));
    CHECK(rejected.state == ice::JobState::failed);
    CHECK(rejected.error.find("unsupported") != std::string::npos);
    CHECK(!rejected.input_fingerprint.empty());
}

TEST_CASE("ngspice initialization errors include simulator diagnostics") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 10e-9;
    request.netlist = {"* missing model include", ".include \"/tmp/ice-does-not-exist-model-12345.lib\"", "V1 in 0 1",
                       "R1 in out 1k", "C1 out 0 1p"};
    ice::SimulationRunner runner;
    const auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::failed);
    CHECK(result.error.find("include") != std::string::npos ||
          result.error.find("ice-does-not-exist") != std::string::npos);
}

TEST_CASE("ngspice fetches the vectors named by the request signals") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"* RC transient", "V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)", "Resistor N14 N15 1k",
                       "Capacitor N15 N16 1n"};
    request.probeSignals = {"N15"};
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(!result.time.empty());
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty()) {
        CHECK(result.traces[0].name == "N15");
        CHECK(result.traces[0].values.size() == result.time.size());
        CHECK(*std::max_element(result.traces[0].values.begin(), result.traces[0].values.end()) > 0.01);
    }
}

TEST_CASE("ngspice adapter supplies a title line for raw netlists") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"V1 out 0 PULSE(0 1 1n 1n 1n 100n 200n)", "R1 out 0 1k"};
    request.probeSignals = {"out"};
    ice::SimulationRunner runner;
    const auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty())
        CHECK(*std::max_element(result.traces.front().values.begin(), result.traces.front().values.end()) > 0.5);
}

TEST_CASE("a run with no resolvable signal names fails with a clear error") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"* RC transient", "V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)", "Resistor N14 N15 1k",
                       "Capacitor N15 N16 1n"};
    request.probeSignals = {"nope"};
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::failed);
    CHECK(result.error.find("nope") != std::string::npos);
}

TEST_CASE("schematic netlist maps symbols, nets, and values to SPICE") {
    ice::Project project;
    project.identity = "net";
    project.name = "net";
    const auto cellId = ice::createSchematicCell(project, "top");
    auto &cell = project.schematicCells.at(cellId);

    auto makeSymbol = [&](const std::string &name, std::vector<std::string> pinNames) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        auto &symbol = cell.symbols.at(symbolId);
        for (const auto &pinName : pinNames) {
            const auto pinId = project.nextId();
            symbol.pins.push_back({pinId, pinName, 0, 0});
        }
        return symbolId;
    };
    auto pinIdOf = [&](ice::ObjectId symbolId, std::size_t index) {
        return cell.symbols.at(symbolId).pins.at(index).id;
    };

    const auto sourceId = makeSymbol("Voltage Source", {"+", "-"});
    const auto resistorId = makeSymbol("Resistor", {"1", "2"});
    const auto groundId = makeSymbol("Ground", {"GND"});
    const auto weirdId = makeSymbol("Mystery Widget", {"a", "b"});

    const auto source = ice::addSchematicInstance(project, cellId, sourceId, "V1", 0, 0);
    const auto resistor = ice::addSchematicInstance(project, cellId, resistorId, "R1", 0, 0);
    ice::addSchematicInstance(project, cellId, groundId, "GND1", 0, 0);
    ice::addSchematicInstance(project, cellId, weirdId, "X1", 0, 0);
    cell.instances.at(source).parameters.push_back({project.nextId(), "value", "PULSE(0 1 1n 1n 1n 100n 200n)"});
    cell.instances.at(resistor).parameters.push_back({project.nextId(), "resistance", "2k"});

    const auto inductorId = makeSymbol("Inductor", {"1", "2"});
    const auto inductor = ice::addSchematicInstance(project, cellId, inductorId, "L1", 0, 0);
    cell.instances.at(inductor).parameters.push_back({project.nextId(), "inductance", "3u"});

    // V1: + -> out, - -> 0 ; R1: 1 -> out, 2 -> 0 ; X1: a -> out, b -> 0.
    const auto netId = project.nextId();
    cell.nets.emplace(netId,
                      ice::Net{netId, "out", {pinIdOf(sourceId, 0), pinIdOf(resistorId, 0), pinIdOf(weirdId, 0)}});
    const auto groundNetId = project.nextId();
    cell.nets.emplace(groundNetId, ice::Net{groundNetId,
                                            "",
                                            {pinIdOf(sourceId, 1), pinIdOf(resistorId, 1), pinIdOf(groundId, 0),
                                             pinIdOf(weirdId, 1)}});

    std::vector<std::string> notes;
    const auto netlist = ice::buildSchematicNetlist(project, cellId, &notes);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : netlist)
            text += line + "\n";
        return text;
    }();

    // Voltage source and resistor are wired out/0 with their values.
    CHECK(joined.find("V1 out 0 PULSE") != std::string::npos);
    // Quantity-named parameters (resistance, inductance) feed the value slot.
    CHECK(joined.find("R1 out 0 2k") != std::string::npos);
    CHECK(joined.find("L1 0 0 3u") != std::string::npos);
    // The unsupported symbol is dropped and reported, not silently netlisted:
    // it appears only in a comment, never as a live X-element.
    CHECK(joined.find("\nX1") == std::string::npos);
    CHECK(joined.find("unsupported: X1 Mystery Widget") != std::string::npos);
    bool reportedUnsupported = false;
    for (const auto &noteText : notes)
        if (noteText.find("unsupported") != std::string::npos)
            reportedUnsupported = true;
    CHECK(reportedUnsupported);
    // No trailing .end: the simulator adapter appends it.
    CHECK(joined.find(".end") == std::string::npos);
}

TEST_CASE("schematic netlist emits transistor, diode, and subcircuit devices") {
    ice::Project project;
    project.identity = "active-netlist";
    project.name = "active-netlist";
    const auto cellId = ice::createSchematicCell(project, "top");
    auto &cell = project.schematicCells.at(cellId);

    const auto makeSymbol = [&](const std::string &name, std::initializer_list<const char *> pins) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        for (const auto *pinName : pins)
            cell.symbols.at(symbolId).pins.push_back({project.nextId(), pinName, 0, 0});
        return symbolId;
    };
    const auto pinId = [&](ice::ObjectId symbolId, const std::string &name) {
        for (const auto &pin : cell.symbols.at(symbolId).pins)
            if (pin.name == name)
                return pin.id;
        return ice::ObjectId{0};
    };
    const auto makeNet = [&](const std::string &name, ice::ObjectId pin) {
        const auto netId = project.nextId();
        cell.nets.emplace(netId, ice::Net{netId, name, {pin}});
        return netId;
    };
    const auto parameter = [&](ice::ObjectId instanceId, const std::string &name, const std::string &value) {
        cell.instances.at(instanceId).parameters.push_back({project.nextId(), name, value});
    };

    const auto nmosSymbol = makeSymbol("IHP NMOS 1.2V", {"G", "D", "S", "B"});
    const auto nmos = ice::addSchematicInstance(project, cellId, nmosSymbol, "M1", 0, 0);
    makeNet("d", pinId(nmosSymbol, "D"));
    makeNet("g", pinId(nmosSymbol, "G"));
    makeNet("s", pinId(nmosSymbol, "S"));
    makeNet("b", pinId(nmosSymbol, "B"));
    parameter(nmos, "model", "sg13_lv_nmos");
    parameter(nmos, "w", "0.2u");
    parameter(nmos, "l", "0.13u");
    parameter(nmos, "m", "1");

    const auto bjtSymbol = makeSymbol("IHP NPN HBT", {"B", "C", "E"});
    const auto bjt = ice::addSchematicInstance(project, cellId, bjtSymbol, "Q1", 0, 0);
    makeNet("c", pinId(bjtSymbol, "C"));
    makeNet("base", pinId(bjtSymbol, "B"));
    makeNet("e", pinId(bjtSymbol, "E"));
    parameter(bjt, "model", "npn13G2");
    parameter(bjt, "m", "2");

    const auto diodeSymbol = makeSymbol("SKY130 Diode 5.5V", {"A", "K"});
    const auto diode = ice::addSchematicInstance(project, cellId, diodeSymbol, "D1", 0, 0);
    makeNet("anode", pinId(diodeSymbol, "A"));
    makeNet("cathode", pinId(diodeSymbol, "K"));
    parameter(diode, "model", "sky130_fd_pr__diode_pw2nd_05v5");

    const auto opampSymbol = makeSymbol("Op Amp", {"+", "-", "OUT"});
    const auto opamp = ice::addSchematicInstance(project, cellId, opampSymbol, "X1", 0, 0);
    makeNet("plus", pinId(opampSymbol, "+"));
    makeNet("minus", pinId(opampSymbol, "-"));
    makeNet("out", pinId(opampSymbol, "OUT"));
    parameter(opamp, "model", "OPAMP");

    const auto genericSymbol = makeSymbol("NMOS", {"G", "D", "S", "B"});
    const auto generic = ice::addSchematicInstance(project, cellId, genericSymbol, "M2", 0, 0);
    CHECK(generic != 0);
    makeNet("d2", pinId(genericSymbol, "D"));
    makeNet("g2", pinId(genericSymbol, "G"));
    makeNet("s2", pinId(genericSymbol, "S"));
    makeNet("b2", pinId(genericSymbol, "B"));

    std::vector<std::string> notes;
    const auto netlist = ice::buildSchematicNetlist(project, cellId, &notes);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";

    CHECK(joined.find("M1 d g s b sg13_lv_nmos W=0.2u L=0.13u M=1") != std::string::npos);
    CHECK(joined.find("Q1 c base e npn13G2 M=2") != std::string::npos);
    CHECK(joined.find("D1 anode cathode sky130_fd_pr__diode_pw2nd_05v5") != std::string::npos);
    CHECK(joined.find("X1 plus minus out OPAMP") != std::string::npos);
    CHECK(joined.find("M2 d2 g2 s2 b2 NMOS") != std::string::npos);
    bool reportedModelFallback = false;
    for (const auto &noteText : notes)
        if (noteText.find("missing model parameter for M2") != std::string::npos)
            reportedModelFallback = true;
    CHECK(reportedModelFallback);
}

TEST_CASE("PDK transistor netlist uses subcircuit calls") {
    ice::Project project;
    project.identity = "pdk-active-netlist";
    project.name = "pdk-active-netlist";
    const auto cellId = ice::createSchematicCell(project, "top", "ihp-sg13g2");
    auto &cell = project.schematicCells.at(cellId);

    const auto makeSymbol = [&](const std::string &name, std::initializer_list<const char *> pins) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        for (const auto *pinName : pins)
            cell.symbols.at(symbolId).pins.push_back({project.nextId(), pinName, 0, 0});
        return symbolId;
    };
    const auto pinId = [&](ice::ObjectId symbolId, const std::string &name) {
        for (const auto &pin : cell.symbols.at(symbolId).pins)
            if (pin.name == name)
                return pin.id;
        return ice::ObjectId{0};
    };
    const auto makeNet = [&](const std::string &name, ice::ObjectId pin) {
        const auto netId = project.nextId();
        cell.nets.emplace(netId, ice::Net{netId, name, {pin}});
    };
    const auto parameter = [&](ice::ObjectId instanceId, const std::string &name, const std::string &value) {
        cell.instances.at(instanceId).parameters.push_back({project.nextId(), name, value});
    };

    const auto nmosSymbol = makeSymbol("IHP NMOS 1.2V", {"G", "D", "S", "B"});
    const auto nmos = ice::addSchematicInstance(project, cellId, nmosSymbol, "M1", 0, 0);
    makeNet("d", pinId(nmosSymbol, "D"));
    makeNet("g", pinId(nmosSymbol, "G"));
    makeNet("s", pinId(nmosSymbol, "S"));
    makeNet("b", pinId(nmosSymbol, "B"));
    parameter(nmos, "model", "sg13_lv_nmos");
    parameter(nmos, "w", "0.2u");
    parameter(nmos, "l", "0.13u");

    const auto bjtSymbol = makeSymbol("IHP NPN HBT", {"B", "C", "E"});
    const auto bjt = ice::addSchematicInstance(project, cellId, bjtSymbol, "Q1", 0, 0);
    makeNet("c", pinId(bjtSymbol, "C"));
    makeNet("base", pinId(bjtSymbol, "B"));
    makeNet("e", pinId(bjtSymbol, "E"));
    parameter(bjt, "model", "npn13G2");

    const auto netlist = ice::buildSchematicNetlist(project, cellId);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";

    CHECK(joined.find("XM1 d g s b sg13_lv_nmos W=0.2u L=0.13u") != std::string::npos);
    CHECK(joined.find("XQ1 c base e 0 npn13G2") != std::string::npos);
}

TEST_CASE("hierarchical schematic instances become subcircuits") {
    ice::Project project;
    project.identity = "hierarchy";
    project.name = "hierarchy";

    const auto addSymbol = [&](ice::ObjectId cellId, const std::string &name,
                               std::initializer_list<const char *> pins) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        auto &symbol = project.schematicCells.at(cellId).symbols.at(symbolId);
        for (const auto *pinName : pins)
            symbol.pins.push_back({project.nextId(), pinName, 0, 0});
        return symbolId;
    };
    const auto pinIdOf = [&](ice::ObjectId cellId, ice::ObjectId symbolId, std::size_t index) {
        return project.schematicCells.at(cellId).symbols.at(symbolId).pins.at(index).id;
    };

    const auto childId = ice::createSchematicCell(project, "filter");
    const auto inputId = addSymbol(childId, "Input", {"A"});
    const auto outputId = addSymbol(childId, "Output", {"Y"});
    const auto resistorId = addSymbol(childId, "Resistor", {"1", "2"});
    const auto childInput = ice::addSchematicInstance(project, childId, inputId, "A", 0, 0);
    const auto childOutput = ice::addSchematicInstance(project, childId, outputId, "Y", 0, 0);
    const auto childResistor = ice::addSchematicInstance(project, childId, resistorId, "R1", 0, 0);
    project.schematicCells.at(childId)
        .instances.at(childResistor)
        .parameters.push_back({project.nextId(), "resistance", "2k"});
    ice::addNet(project, childId, "", {pinIdOf(childId, inputId, 0), pinIdOf(childId, resistorId, 0)});
    ice::addNet(project, childId, "", {pinIdOf(childId, outputId, 0), pinIdOf(childId, resistorId, 1)});
    CHECK(childInput != 0);
    CHECK(childOutput != 0);

    const auto parentId = ice::createSchematicCell(project, "top");
    const auto blockId = addSymbol(parentId, "filter", {"A", "Y"});
    project.schematicCells.at(parentId).symbols.at(blockId).sourceCell = childId;
    const auto sourceId = addSymbol(parentId, "Voltage Source", {"+", "-"});
    const auto block = ice::addSchematicInstance(project, parentId, blockId, "X1", 0, 0);
    const auto source = ice::addSchematicInstance(project, parentId, sourceId, "V1", 0, 0);
    project.schematicCells.at(parentId).instances.at(source).parameters.push_back({project.nextId(), "value", "1"});
    ice::addNet(project, parentId, "in", {pinIdOf(parentId, blockId, 0), pinIdOf(parentId, sourceId, 0)});
    ice::addNet(project, parentId, "out", {pinIdOf(parentId, blockId, 1), pinIdOf(parentId, sourceId, 1)});
    CHECK(block != 0);

    std::vector<std::string> notes;
    const auto netlist = ice::buildSchematicNetlist(project, parentId, &notes);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";

    CHECK(joined.find(".subckt filter A Y") != std::string::npos);
    CHECK(joined.find("R1 A Y 2k") != std::string::npos);
    CHECK(joined.find(".ends filter") != std::string::npos);
    CHECK(joined.find("X1 in out filter") != std::string::npos);
    CHECK(joined.find("V1 in out 1") != std::string::npos);
    CHECK(notes.empty());
}

TEST_CASE("layout pins and nets round-trip with required metadata") {
    ice::Project project;
    project.identity = "pins";
    project.name = "pins";
    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto layout = ice::createLayoutCell(project, "top");
    const auto net = ice::createLayoutNet(project, layout, "VIN");
    const auto port = ice::addPort(project, layout, "VIN", layer, {0, 0, 400, 400}, ice::PortDirection::input,
                                   ice::PortSide::left, net, "VIN");
    CHECK(project.validate().empty());

    const auto &stored = project.layoutCells.at(layout).ports.at(port);
    CHECK(stored.name == "VIN");
    CHECK(stored.direction == ice::PortDirection::input);
    CHECK(stored.side == ice::PortSide::left);
    CHECK(stored.net == net);
    CHECK(stored.label == "VIN");
    CHECK_THROWS_AS(ice::addPort(project, layout, "vin", layer, {0, 0, 100, 100}), ice::ProjectError);

    const auto serialized = project.serialize();
    auto oldSchema = serialized;
    const auto version = oldSchema.find("\"schema_version\": 2");
    CHECK(version != std::string::npos);
    if (version != std::string::npos)
        oldSchema.replace(version, std::string("\"schema_version\": 2").size(), "\"schema_version\": 1");
    CHECK_THROWS_AS(ice::Project::deserialize(oldSchema), ice::ProjectError);
    const auto roundTrip = ice::Project::deserialize(serialized);
    const auto &roundTripPort = roundTrip.layoutCells.at(layout).ports.at(port);
    CHECK(roundTripPort.name == "VIN");
    CHECK(roundTripPort.direction == ice::PortDirection::input);
    CHECK(roundTripPort.side == ice::PortSide::left);
    CHECK(roundTripPort.net == net);
    CHECK(roundTrip.layoutCells.at(layout).nets.at(net).name == "VIN");

    ice::movePort(project, layout, port, 100, 200);
    CHECK(project.layoutCells.at(layout).ports.at(port).rectangle.x == 100);
    ice::resizePort(project, layout, port, {100, 200, 800, 600});
    CHECK(project.layoutCells.at(layout).ports.at(port).rectangle.width == 800);
    ice::assignPortNet(project, layout, port, 0);
    CHECK(project.layoutCells.at(layout).ports.at(port).net == 0);
    ice::assignPortNet(project, layout, port, net);
    CHECK(project.layoutCells.at(layout).ports.at(port).net == net);
    ice::removePort(project, layout, port);
    CHECK(project.layoutCells.at(layout).ports.empty());
}

TEST_CASE("waveform and current source symbols generate SPICE source expressions") {
    ice::Project project;
    project.identity = "sources";
    project.name = "sources";
    const auto cellId = ice::createSchematicCell(project, "top");
    auto &cell = project.schematicCells.at(cellId);

    auto addSource = [&](const std::string &name) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        auto &symbol = cell.symbols.at(symbolId);
        for (const auto *pinName : {"+", "-"}) {
            const auto pinId = project.nextId();
            symbol.pins.push_back({pinId, pinName, 0, 0});
        }
        return symbolId;
    };
    auto pinIdOf = [&](ice::ObjectId symbolId, std::size_t index) {
        return cell.symbols.at(symbolId).pins.at(index).id;
    };
    auto setParameters = [&](ice::ObjectId instanceId,
                             std::initializer_list<std::pair<std::string, std::string>> values) {
        for (const auto &[name, value] : values)
            cell.instances.at(instanceId).parameters.push_back({project.nextId(), name, value});
    };

    const auto pulseId = addSource("Pulse Voltage Source");
    const auto sineId = addSource("Sine Voltage Source");
    const auto currentId = addSource("Current Source");
    const auto pulseDefaultsId = addSource("Pulse Voltage Source");
    const auto sineDefaultsId = addSource("Sine Voltage Source");
    const auto groundId = addSource("Ground");

    ice::addSchematicInstance(project, cellId, groundId, "GND1", 0, 0);
    const auto pulse = ice::addSchematicInstance(project, cellId, pulseId, "V1", 0, 0);
    const auto sine = ice::addSchematicInstance(project, cellId, sineId, "V2", 0, 0);
    const auto current = ice::addSchematicInstance(project, cellId, currentId, "I1", 0, 0);
    ice::addSchematicInstance(project, cellId, pulseDefaultsId, "V3", 0, 0);
    ice::addSchematicInstance(project, cellId, sineDefaultsId, "V4", 0, 0);
    setParameters(pulse, {{"initial_voltage", "-1"},
                          {"pulsed_voltage", "2.5"},
                          {"delay_time", "1u"},
                          {"rise_time", "2u"},
                          {"fall_time", "3u"},
                          {"pulse_width", "4m"},
                          {"period", "5m"},
                          {"pulse_count", "3"}});
    setParameters(sine, {{"offset_voltage", "1"},
                         {"amplitude", "2"},
                         {"frequency", "3k"},
                         {"delay_time", "4u"},
                         {"damping_factor", "5"},
                         {"phase", "6"}});
    setParameters(current, {{"current", "7m"}});

    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, ice::Net{outNet,
                                       "out",
                                       {pinIdOf(pulseId, 0), pinIdOf(sineId, 0), pinIdOf(currentId, 0),
                                        pinIdOf(pulseDefaultsId, 0), pinIdOf(sineDefaultsId, 0)}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet,
                      ice::Net{groundNet,
                               "",
                               {pinIdOf(pulseId, 1), pinIdOf(sineId, 1), pinIdOf(currentId, 1),
                                pinIdOf(pulseDefaultsId, 1), pinIdOf(sineDefaultsId, 1), pinIdOf(groundId, 0)}});

    const auto netlist = ice::buildSchematicNetlist(project, cellId);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : netlist)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find("V1 out 0 PULSE(-1 2.5 1u 2u 3u 4m 5m 3)") != std::string::npos);
    CHECK(joined.find("V2 out 0 SIN(1 2 3k 4u 5 6)") != std::string::npos);
    CHECK(joined.find("I1 out 0 7m") != std::string::npos);
    // Missing parameters fall back to the symbol's documented defaults.
    CHECK(joined.find("V3 out 0 PULSE(0 1 0 1u 1u 0.5 1 0)") != std::string::npos);
    CHECK(joined.find("V4 out 0 SIN(0 1 1 0 0 0)") != std::string::npos);
    CHECK(ice::spiceElementForSymbol("Current Source") == 'I');
    CHECK(ice::spiceElementForSymbol("Pulse Voltage Source") == 'V');
    CHECK(ice::spiceElementForSymbol("Sine Voltage Source") == 'V');
    CHECK(ice::spiceElementForSymbol("IHP NMOS 1.2V") == 'M');
    CHECK(ice::spiceElementForSymbol("SKY130 PMOS 1.8V") == 'M');
    CHECK(ice::spiceElementForSymbol("IHP NPN HBT") == 'Q');
    CHECK(ice::spiceElementForSymbol("Op Amp") == 'X');
}

TEST_CASE("generated source expressions are accepted by ngspice") {
    auto run = [](const std::vector<std::string> &netlist, const std::string &probe) {
        ice::SimulationRequest request;
        request.analysis = "transient";
        request.step = 0.01;
        request.stop = 2.0;
        // ngspice treats the first netlist line as the circuit title, so make
        // it an explicit comment before the source netlist.
        request.netlist = {"* source expression test"};
        request.netlist.insert(request.netlist.end(), netlist.begin(), netlist.end());
        request.probeSignals = {probe};
        ice::SimulationRunner runner;
        return runner.run(request, std::chrono::seconds(5));
    };

    const auto pulse = run({"V1 out 0 DC 0 PULSE(0 1 0 1u 1u 0.5 1)", "R1 out 0 1k"}, "out");
    CHECK(pulse.state == ice::JobState::succeeded);
    CHECK(pulse.traces.size() == 1);
    if (!pulse.traces.empty()) {
        const auto &values = pulse.traces.front().values;
        CHECK(!values.empty());
        CHECK(*std::max_element(values.begin(), values.end()) > 0.9);
    }

    const auto sine = run({"V1 out 0 SIN(0 1 1 0 0 0)", "R1 out 0 1k"}, "out");
    CHECK(sine.state == ice::JobState::succeeded);
    CHECK(sine.traces.size() == 1);
    if (!sine.traces.empty()) {
        const auto &values = sine.traces.front().values;
        CHECK(!values.empty());
        CHECK(*std::max_element(values.begin(), values.end()) > 0.9);
        CHECK(*std::min_element(values.begin(), values.end()) < -0.9);
    }

    const auto current = run({"I1 0 out 1m", "R1 out 0 1k"}, "out");
    CHECK(current.state == ice::JobState::succeeded);
    CHECK(current.traces.size() == 1);
}

TEST_CASE("package manifests and lockfiles are reproducible") {
    auto repositoryRoot = std::filesystem::temp_directory_path() / "ice-package-test";
    std::filesystem::remove_all(repositoryRoot);
    std::filesystem::create_directories(repositoryRoot / "example-lib");
    std::filesystem::create_directories(repositoryRoot / "example-pdk" / "layout");
    std::ofstream(repositoryRoot / "example-pdk/package.toml")
        << "[package]\nname = \"example.pdk\"\nversion = \"1.0.0\"\n\n[provides]\npdk = true\n"
           "\n[pdk]\ndisplay_name = \"Example PDK\"\ndefault_schematic = \"example_top\"\n"
           "default_stack = \"base\"\n"
           "\n[pdk.implementation]\nengine = \"openroad\"\nsynthesis = \"yosys\"\nmin_openroad = \"2.0\"\n"
           "site = \"example_site\"\ntech_lef = \"implementation/tech.lef\"\n"
           "cell_lef = \"implementation/cells.lef\"\nrouting_min_layer = \"Metal1\"\n"
           "routing_max_layer = \"Metal2\"\n"
           "\n[pdk.implementation.liberty]\ntt = [\"implementation/tt.lib\"]\n"
           "\n[pdk.implementation.orfs]\nplatform = \"example\"\nmetal_option = \"base\"\n"
           "\n[[pdk.stack]]\nid = \"base\"\ndisplay_name = \"Base stack\"\n"
           "layers = [\"diff.drawing\", \"metal1.drawing\"]\n"
           "\n[[pdk.layer]]\nname = \"diff.drawing\"\nnumber = 65\ndatatype = 20\nminimum_width = 150\n"
           "\n[[pdk.layer]]\nname = \"metal1.drawing\"\nnumber = 68\ndatatype = 20\nminimum_width = 230\n";
    std::ofstream(repositoryRoot / "example-pdk/layout/layer-styles.json")
        << R"({"schema_version": 1, "layers": [{"name": "diff", "color": "#123456", "pattern": "Diagonal /", "visible": true}]})";
    std::ofstream(repositoryRoot / "example-lib/package.toml")
        << "[package]\nname = \"example.lib\"\nversion = \"1.0.0\"\n\n[dependencies]\n\"example.pdk\" = "
           "\">=1.0.0\"\n\n[provides]\nmodels = true\nsymbols = true\n";
    auto packageRoot = repositoryRoot / "example-lib";
    auto package = ice::PackageManager::readManifest(packageRoot);
    CHECK(package.name == "example.lib");
    CHECK(package.dependencies.at("example.pdk") == ">=1.0.0");
    CHECK((ice::PackageManager::capabilities(package) == std::vector<std::string>{"models", "symbols"}));
    const auto pdk = ice::PackageManager::readManifest(repositoryRoot / "example-pdk");
    CHECK(pdk.pdk.has_value());
    if (pdk.pdk) {
        CHECK(pdk.pdk->displayName == "Example PDK");
        CHECK(pdk.pdk->defaultSchematic == "example_top");
        CHECK(pdk.pdk->layers.size() == 2);
        CHECK(pdk.pdk->layers.front().name == "diff.drawing");
        CHECK(pdk.pdk->layers.front().number == 65);
        CHECK(pdk.pdk->layers.front().datatype == 20);
        CHECK(pdk.pdk->layers.front().minimumWidth == 150);
        CHECK(pdk.pdk->layers.back().minimumWidth == 230);
        CHECK(pdk.pdk->defaultStack == "base");
        CHECK(pdk.pdk->stacks.size() == 1);
        if (!pdk.pdk->stacks.empty()) {
            CHECK(pdk.pdk->stacks.front().id == "base");
            CHECK(pdk.pdk->stacks.front().displayName == "Base stack");
            CHECK(pdk.pdk->stacks.front().layers.size() == 2);
        }
        CHECK(ice::resolvePdkStack(*pdk.pdk, "") == "base");
        CHECK(ice::resolvePdkStack(*pdk.pdk, "base") == "base");
        CHECK_THROWS_AS(ice::resolvePdkStack(*pdk.pdk, "missing"), ice::ProjectError);
        CHECK(pdk.pdk->implementation.has_value());
        if (pdk.pdk->implementation) {
            const auto &implementation = *pdk.pdk->implementation;
            CHECK(implementation.engine == "openroad");
            CHECK(implementation.synthesis == "yosys");
            CHECK(implementation.minOpenRoad == "2.0");
            CHECK(implementation.orfsPlatform == "example");
            CHECK(implementation.orfsMetalOption == "base");
            CHECK(implementation.files.has_value());
            if (implementation.files) {
                CHECK(implementation.files->site == "example_site");
                CHECK(implementation.files->techLef == "implementation/tech.lef");
                CHECK(implementation.files->cellLef == "implementation/cells.lef");
                CHECK(implementation.files->routingMinLayer == "Metal1");
                CHECK(implementation.files->routingMaxLayer == "Metal2");
                CHECK(implementation.files->liberty.at("tt").front() == "implementation/tt.lib");
            }
        }
        CHECK(pdk.pdk->layerStyles.size() == 1);
        if (!pdk.pdk->layerStyles.empty()) {
            CHECK(pdk.pdk->layerStyles.front().name == "diff");
            CHECK(pdk.pdk->layerStyles.front().color == "#123456");
            CHECK(pdk.pdk->layerStyles.front().pattern == "Diagonal /");
            CHECK(pdk.pdk->layerStyles.front().visible == 1);
        }
    }
    auto lock = ice::PackageManager::resolve({repositoryRoot}, {{"example.lib", "*"}}, "0.1.0");
    CHECK(lock.packages.size() == 2);
    auto serialized = ice::serializeLockfile(lock);
    CHECK(serialized == ice::serializeLockfile(lock));
    CHECK(serialized.find("content_hash = \"sha256:") != std::string::npos);
    CHECK(serialized.find("pdk_id = \"example.pdk\"") != std::string::npos);
    const auto parsed = ice::PackageManager::readLockfile([&] {
        const auto path = repositoryRoot / "ice.lock";
        ice::PackageManager::writeLockfile(path, lock);
        return path;
    }());
    CHECK(parsed.packages.size() == 2);
    CHECK(std::any_of(parsed.packages.begin(), parsed.packages.end(),
                      [](const ice::LockedPackage &package) { return package.pdkId == "example.pdk"; }));
    std::filesystem::remove_all(repositoryRoot);
}

TEST_CASE("package indexes discover submodules and logos") {
    const auto root = std::filesystem::temp_directory_path() / "ice-package-index-test";
    std::filesystem::remove_all(root);
    const auto packageRoot = root / "packages" / "example.lib";
    std::filesystem::create_directories(packageRoot / ".github");
    std::ofstream(root / ".gitmodules") << "[submodule \"example.lib\"]\n"
                                           "    path = packages/example.lib\n"
                                           "    url = https://example.invalid/example.lib.git\n"
                                           "    branch = dev\n";
    std::ofstream(packageRoot / "package.toml") << "[package]\n"
                                                   "schema_version = 1\n"
                                                   "name = \"example.lib\"\n"
                                                   "version = \"1.2.0\"\n"
                                                   "description = \"Example indexed package\"\n"
                                                   "\n[provides]\n"
                                                   "symbols = true\n";
    std::ofstream(packageRoot / ".github" / "logo.png") << "not a real image, only presence is tested";

    const auto entries = ice::PackageManager::scanPackageIndex(root);
    CHECK(entries.size() == 1);
    if (entries.size() == 1) {
        CHECK(entries.front().spec.name == "example.lib");
        CHECK(entries.front().repositoryUrl == "https://example.invalid/example.lib.git");
        CHECK(entries.front().repositoryRevision == "dev");
        CHECK(entries.front().logo == packageRoot / ".github" / "logo.png");
    }
    CHECK(ice::PackageManager::packageLogo(packageRoot) == packageRoot / ".github" / "logo.png");
    std::filesystem::remove_all(root);
}

TEST_CASE("installed packages can be uninstalled") {
    const auto root = std::filesystem::temp_directory_path() / "ice-package-uninstall-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "example.pkg");
    std::ofstream(root / "example.pkg" / "package.toml") << "[package]\nname = \"example.pkg\"\nversion = \"1.0.0\"\n";
    CHECK(std::filesystem::exists(root / "example.pkg"));
    ice::PackageManager::uninstallPackage(root, "example.pkg");
    CHECK(!std::filesystem::exists(root / "example.pkg"));
    CHECK_THROWS_AS(ice::PackageManager::uninstallPackage(root, "missing.pkg"), ice::ProjectError);
    CHECK_THROWS_AS(ice::PackageManager::uninstallPackage(root, "../escape"), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("schematics carry PDKs and projects carry package dependencies") {
    ice::Project project;
    project.identity = "pdk-project";
    project.name = "pdk-project";
    const auto generic = ice::createSchematicCell(project, "generic_top");
    const auto packaged = ice::createSchematicCell(project, "ihp_top", "ihp.sg13g2");
    CHECK(project.schematicCells.at(generic).pdk == ice::genericPdkId);
    CHECK(project.schematicCells.at(packaged).pdk == "ihp.sg13g2");
    project.packageDependencies = {"ihp.sg13g2"};
    CHECK(project.validate().empty());

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.packageDependencies == std::vector<std::string>{"ihp.sg13g2"});
    CHECK(roundTrip.schematicCells.at(generic).pdk == ice::genericPdkId);
    CHECK(roundTrip.schematicCells.at(packaged).pdk == "ihp.sg13g2");

    auto duplicated = project;
    duplicated.packageDependencies.push_back("ihp.sg13g2");
    CHECK(!duplicated.validate().empty());
    CHECK_THROWS_AS(ice::createSchematicCell(project, "empty_pdk", ""), ice::ProjectError);
    CHECK_THROWS_AS(ice::createSchematicCell(project, "bad_pdk", "../escape"), ice::ProjectError);
}

TEST_CASE("file symbol library loads and validates") {
    const auto root = std::filesystem::temp_directory_path() / "ice-symbols-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "resistor.svg") << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 8\">"
                                            "<path d=\"M0 4h32\" stroke=\"black\"/></svg>";
    std::ofstream(root / "resistor.toml")
        << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\ncategory = \"Passive\"\n"
           "label_x = 0\nlabel_y = 12\n"
           "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
           "\n[[pin]]\nname = \"2\"\nx = 16\ny = 0\ndirection = \"inout\"\n";
    const auto symbols = ice::SymbolLibrary::load(root);
    CHECK(symbols.size() == 1);
    CHECK(symbols.front().name == "Resistor");
    CHECK(symbols.front().category == "Passive");
    CHECK(symbols.front().svg.find("<svg") != std::string::npos);
    CHECK(symbols.front().label.has_value());
    CHECK(symbols.front().label->x == 0);
    CHECK(symbols.front().label->y == 12);
    CHECK(symbols.front().pins.size() == 2);
    CHECK(symbols.front().pins.front().direction == ice::PinDirection::left);
    CHECK(symbols.front().pins.back().direction == ice::PinDirection::right);

    const auto missing = root / "missing";
    std::filesystem::create_directories(missing);
    std::ofstream(missing / "broken.toml") << "[symbol]\nname = \"Broken\"\nartwork = \"absent.svg\"\n"
                                              "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(missing), ice::ProjectError);

    const auto scripted = root / "scripted";
    std::filesystem::create_directories(scripted);
    std::ofstream(scripted / "evil.svg") << "<svg><script>alert(1)</script></svg>";
    std::ofstream(scripted / "evil.toml") << "[symbol]\nname = \"Evil\"\nartwork = \"evil.svg\"\n"
                                             "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(scripted), ice::ProjectError);

    const auto fractional = root / "fractional";
    std::filesystem::create_directories(fractional);
    std::ofstream(fractional / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(fractional / "resistor.toml") << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = -16.5\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(fractional), ice::ProjectError);

    const auto duplicated = root / "duplicated";
    std::filesystem::create_directories(duplicated);
    std::ofstream(duplicated / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(duplicated / "resistor.toml") << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = 16\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(duplicated), ice::ProjectError);

    const auto nameless = root / "nameless";
    std::filesystem::create_directories(nameless);
    std::ofstream(nameless / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(nameless / "resistor.toml")
        << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\ncategory = \"Passive\"\n"
           "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
           "\n[[pin]]\nname = \"2\"\nx = 16\ny = 0\ndirection = \"inout\"\n"
           "\n[[parameter]]\nname = \"\"\nvalue = \"1k\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(nameless), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("symbols can reference built-in generic artwork") {
    const auto root = std::filesystem::temp_directory_path() / "ice-generic-artwork-test";
    std::filesystem::remove_all(root);
    const auto builtin = root / "builtin";
    const auto package = root / "package";
    const auto traversal = root / "traversal";
    std::filesystem::create_directories(builtin);
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(traversal);
    std::ofstream(builtin / "shared.svg") << "<svg viewBox=\"0 0 10 10\"><path d=\"M0 0h10\"/></svg>";
    std::ofstream(package / "referenced.toml") << "[symbol]\nname = \"Referenced\"\nartwork = \"generic:shared.svg\"\n"
                                                  "\n[[pin]]\nname = \"1\"\nx = -5\ny = 0\ndirection = \"inout\"\n"
                                                  "\n[[pin]]\nname = \"2\"\nx = 5\ny = 0\ndirection = \"inout\"\n";
    const auto symbols = ice::SymbolLibrary::load(package, builtin);
    CHECK(symbols.size() == 1);
    CHECK(symbols.front().svg.find("M0 0h10") != std::string::npos);
    CHECK_THROWS_AS(ice::SymbolLibrary::load(package), ice::ProjectError);

    std::ofstream(traversal / "escape.toml") << "[symbol]\nname = \"Escape\"\nartwork = \"generic:../shared.svg\"\n"
                                                "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(traversal, builtin), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("shipped file symbol library loads") {
    const auto libraryRoot = std::filesystem::path(ICE_SOURCE_DIR) / "symbols";
    const auto symbols = ice::SymbolLibrary::load(libraryRoot);
    CHECK(symbols.size() == 13);
    std::map<std::string, std::size_t> pinCounts;
    std::map<std::string, std::map<std::string, std::string>> defaults;
    for (const auto &symbol : symbols) {
        CHECK(!symbol.svg.empty());
        CHECK(!symbol.pins.empty());
        CHECK(symbol.label.has_value());
        pinCounts[symbol.name] = symbol.pins.size();
        for (const auto &parameter : symbol.defaultParameters)
            defaults[symbol.name][parameter.name] = parameter.value;
    }
    CHECK(pinCounts["Resistor"] == 2);
    CHECK(pinCounts["Capacitor"] == 2);
    CHECK(pinCounts["Diode"] == 2);
    CHECK(pinCounts["Voltage Source"] == 2);
    CHECK(pinCounts["Pulse Voltage Source"] == 2);
    CHECK(pinCounts["Sine Voltage Source"] == 2);
    CHECK(pinCounts["Current Source"] == 2);
    CHECK(pinCounts["Ground"] == 1);
    CHECK(pinCounts["Op Amp"] == 3);
    CHECK(pinCounts["NMOS"] == 4);
    CHECK(pinCounts["Inductor"] == 2);
    CHECK(pinCounts["Input"] == 1);
    CHECK(pinCounts["Output"] == 1);
    // Passive and source symbols seed their editable default parameters.
    CHECK(defaults["Resistor"]["resistance"] == "1k");
    CHECK(defaults["Capacitor"]["capacitance"] == "1n");
    CHECK(defaults["Inductor"]["inductance"] == "1u");
    CHECK(defaults["Voltage Source"]["voltage"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["initial_voltage"] == "0");
    CHECK(defaults["Pulse Voltage Source"]["pulsed_voltage"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["rise_time"] == "1u");
    CHECK(defaults["Pulse Voltage Source"]["pulse_width"] == "0.5");
    CHECK(defaults["Pulse Voltage Source"]["period"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["pulse_count"] == "0");
    CHECK(defaults["Sine Voltage Source"]["offset_voltage"] == "0");
    CHECK(defaults["Sine Voltage Source"]["amplitude"] == "1");
    CHECK(defaults["Sine Voltage Source"]["frequency"] == "1");
    CHECK(defaults["Sine Voltage Source"]["phase"] == "0");
    CHECK(defaults["Current Source"]["current"] == "1m");
}

TEST_CASE("package layout device libraries load") {
    const auto repositoryRoot = std::filesystem::path(ICE_SOURCE_DIR).parent_path();
    struct PackageLibrary {
        const char *directory;
        const char *name;
        const char *pdk;
        const char *prefix;
        const char *device;
        int drawingLayer;
        int drawingDatatype;
        int purposeDatatype;
    };
    const std::vector<PackageLibrary> packages = {
        {"ice-pkg-ihp-sg13g2", "IHP SG13G2", "ihp-sg13g2", "IHP ", "nmos", 8, 0, 2},
        {"ice-pkg-sky130", "SkyWater SKY130", "sky130A", "SKY130 ", "nfet", 68, 20, 16},
        {"ice-pkg-gf180mcu", "GlobalFoundries GF180MCU", "gf180mcuA", "GF180 ", "nmos", 34, 0, 4},
    };
    for (const auto &package : packages) {
        const auto path = repositoryRoot / package.directory / "layout" / "devices.json";
        if (!std::filesystem::is_regular_file(path))
            continue;
        const auto library = ice::LayoutDeviceLibrary::load(path);
        CHECK(library.name == package.name);
        CHECK(library.pdk == package.pdk);
        CHECK(library.cellPrefix == package.prefix);
        CHECK(!library.cells.empty());
        const auto hasStream = [&](int number, int datatype) {
            return std::any_of(library.layers.begin(), library.layers.end(),
                               [&](const auto &layer) { return layer.number == number && layer.datatype == datatype; });
        };
        CHECK(hasStream(package.drawingLayer, package.drawingDatatype));
        CHECK(hasStream(package.drawingLayer, package.purposeDatatype));
        const auto manifest = ice::PackageManager::readManifest(repositoryRoot / package.directory);
        CHECK(manifest.pdk.has_value());
        if (manifest.pdk) {
            const auto drawing =
                std::find_if(manifest.pdk->layers.begin(), manifest.pdk->layers.end(), [&](const auto &layer) {
                    return layer.number == package.drawingLayer && layer.datatype == package.drawingDatatype;
                });
            CHECK(drawing != manifest.pdk->layers.end());
        }
        const auto findCell = [&](const std::string &name) -> const ice::LayoutDeviceCell * {
            for (const auto &cell : library.cells)
                if (cell.name == name)
                    return &cell;
            return nullptr;
        };
        const auto *device = findCell(package.device);
        CHECK(device != nullptr);
        if (device) {
            CHECK(device->generator.has_value());
            CHECK(!device->parameters.empty());
            CHECK(!device->shapes.empty());
            CHECK(device->bounds.valid());
        }
    }
}

TEST_CASE("shipped IHP layout device library loads") {
    const auto libraryPath = std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "ihp.sg13g2.json";
    const auto library = ice::LayoutDeviceLibrary::load(libraryPath);
    CHECK(library.name == "IHP SG13G2");
    CHECK(library.pdk == "ihp-sg13g2");
    CHECK(library.cells.size() >= 100);
    CHECK(!library.layers.empty());
    for (const auto &cell : library.cells) {
        if (!cell.parameters.empty())
            CHECK(cell.generator.has_value());
    }
    const auto findCell = [&](const std::string &name) -> const ice::LayoutDeviceCell * {
        for (const auto &cell : library.cells)
            if (cell.name == name)
                return &cell;
        return nullptr;
    };
    const auto *nmos = findCell("nmos");
    CHECK(nmos != nullptr);
    if (nmos) {
        CHECK(nmos->category == "Transistors");
        CHECK(!nmos->shapes.empty());
        CHECK(nmos->ports.size() == 3);
        const auto hasPort = [&](const std::string &name) {
            return std::any_of(nmos->ports.begin(), nmos->ports.end(),
                               [&](const ice::LayoutDevicePort &port) { return port.name == name; });
        };
        CHECK(hasPort("S"));
        CHECK(hasPort("G"));
        CHECK(hasPort("D"));
        CHECK(nmos->bounds.valid());
        CHECK(nmos->parameters.size() == 9);
        CHECK(nmos->generator.has_value());
        if (nmos->generator) {
            CHECK(nmos->generator->module == "nmos_code");
            CHECK(nmos->generator->class_name == "nmos");
        }
        if (nmos->parameters.size() == 9) {
            CHECK(nmos->parameters[0].name == "model");
            CHECK(nmos->parameters[0].value == "sg13_lv_nmos");
            CHECK(nmos->parameters[1].name == "w");
            CHECK(nmos->parameters[1].value == "0.15u");
            CHECK(!nmos->parameters[1].description.empty());
        }
    }
    const auto *inverter = findCell("sg13g2_inv_1");
    CHECK(inverter != nullptr);
    if (inverter) {
        CHECK(inverter->category == "Standard Cells");
        CHECK(!inverter->shapes.empty());
        CHECK(inverter->parameters.empty());
        CHECK(!inverter->generator.has_value());
    }
}

TEST_CASE("generated layout device geometry parses") {
    const auto geometry = ice::parseLayoutDeviceGeometry(
        R"({"bounds":[0,-1,10,20],"shapes":[{"layer":1,"datatype":2,"points":[[0,-1],[10,-1],[10,20],[0,20]]}],)"
        R"("ports":[{"name":"G","layer":1,"datatype":0,"rectangle":{"x":0,"y":0,"width":2,"height":3},)"
        R"("direction":"input","side":"left","label":"G"}]})");
    CHECK(geometry.bounds.x == 0);
    CHECK(geometry.bounds.y == -1);
    CHECK(geometry.bounds.width == 10);
    CHECK(geometry.bounds.height == 21);
    CHECK(geometry.shapes.size() == 1);
    CHECK(geometry.shapes.front().layer == 1);
    CHECK(geometry.shapes.front().datatype == 2);
    CHECK(geometry.shapes.front().polygon.size() == 4);
    CHECK(geometry.ports.size() == 1);
    CHECK(geometry.ports.front().name == "G");
    CHECK(geometry.ports.front().direction == ice::PortDirection::input);
    CHECK(geometry.ports.front().side == ice::PortSide::left);
    CHECK(geometry.ports.front().rectangle.width == 2);
    CHECK_THROWS_AS(ice::parseLayoutDeviceGeometry(R"({"bounds":[0,0,0,0],"shapes":[]})"), ice::ProjectError);
}

TEST_CASE("layout instances survive serialization") {
    ice::Project project;
    project.identity = "layout-instances";
    project.name = "layout-instances";
    const auto childId = ice::createLayoutCell(project, "device", "ihp.sg13g2");
    const auto parentId = ice::createLayoutCell(project, "top", "ihp.sg13g2");
    project.layoutCells.at(childId).library = true;
    project.layoutCells.at(childId).device = "nmos";
    const auto instanceId = project.nextId();
    project.layoutCells.at(parentId).instances.emplace(
        instanceId, ice::LayoutInstance{instanceId, childId, "device_1", 120, -80, ice::Orientation::r0});
    const auto widthId = project.nextId();
    project.layoutCells.at(childId).parameters.push_back({widthId, "width", "0.15"});
    CHECK(project.validate().empty());
    auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.layoutCells.at(childId).library);
    CHECK(roundTrip.layoutCells.at(childId).device == "nmos");
    CHECK(!roundTrip.layoutCells.at(parentId).library);
    CHECK(roundTrip.layoutCells.at(childId).parameters.size() == 1);
    CHECK(roundTrip.layoutCells.at(childId).parameters.front().id == widthId);
    CHECK(roundTrip.layoutCells.at(childId).parameters.front().name == "width");
    CHECK(roundTrip.layoutCells.at(childId).parameters.front().value == "0.15");
    CHECK(roundTrip.nextId() > widthId);
    const auto &instance = roundTrip.layoutCells.at(parentId).instances.at(instanceId);
    CHECK(instance.cell == childId);
    CHECK(instance.name == "device_1");
    CHECK(instance.x == 120);
    CHECK(instance.y == -80);
}

TEST_CASE("procedural layouts own a generated static layout") {
    ice::Project project;
    project.identity = "procedural";
    project.name = "procedural";
    const auto proceduralId = ice::createProceduralLayout(project, "generated_top", ice::genericPdkId,
                                                          "layout.layer(\"Metal1\").rectangle(0, 0, 100, 100)\n");
    const auto &procedural = project.proceduralLayouts.at(proceduralId);
    CHECK(procedural.name == "generated_top");
    CHECK(procedural.script.find("Metal1") != std::string::npos);
    CHECK(procedural.generatedCell != 0);
    const auto &generated = project.layoutCells.at(procedural.generatedCell);
    CHECK(generated.name == "generated_top (generated)");
    CHECK(generated.generatedBy == proceduralId);
    CHECK(project.validate().empty());

    const auto serialized = project.serialize();
    auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.proceduralLayouts.at(proceduralId).generatedCell == procedural.generatedCell);
    CHECK(roundTrip.layoutCells.at(procedural.generatedCell).generatedBy == proceduralId);
    CHECK(roundTrip.nextId() > proceduralId);

    CHECK_THROWS_AS(ice::createProceduralLayout(project, "generated_top"), ice::ProjectError);
    const auto secondId = ice::createProceduralLayout(project, "generated_second");
    CHECK(project.proceduralLayouts.at(secondId).name == "generated_second");
    CHECK(project.proceduralLayouts.at(secondId).generatedCell != procedural.generatedCell);
    CHECK(project.validate().empty());
}

TEST_CASE("geometry even-odd fill splits holes into fillable polygons") {
    ice::GeometryKernel kernel;
    const ice::MultiPolygon contours = {
        {{0, 0}, {100, 0}, {100, 100}, {0, 100}},
        {{25, 25}, {75, 25}, {75, 75}, {25, 75}},
    };
    const auto filled = kernel.fillEvenOdd(contours);
    CHECK(filled.size() > 1);
    long double area = 0.0L;
    for (const auto &polygon : filled) {
        CHECK(ice::GeometryKernel::valid(polygon));
        area += std::abs(polygonArea(polygon));
    }
    CHECK(area == 7500.0L);
}

TEST_CASE("layout shape boolean operations replace their operands") {
    ice::Project project;
    project.identity = "boolean-demo";
    project.name = "boolean-demo";
    const auto layerId = project.nextId();
    project.layers.emplace(layerId, ice::Layer{layerId, "metal1", 1});
    const auto cellId = ice::createLayoutCell(project, "top");
    const auto first = ice::addRectangle(project, cellId, layerId, {0, 0, 100, 100});
    const auto second = ice::addRectangle(project, cellId, layerId, {50, 0, 100, 100});
    const auto unionIds = ice::booleanLayoutShapes(project, cellId, {first, second}, ice::BooleanOperation::union_);
    const auto &cell = project.layoutCells.at(cellId);
    CHECK(unionIds.size() == 1);
    CHECK(cell.shapes.size() == 1);
    const auto &united = cell.shapes.at(unionIds.front());
    CHECK(united.name == "Union 1");
    CHECK(united.polygon.size() >= 4);
    CHECK(united.rectangle.x == 0);
    CHECK(united.rectangle.y == 0);
    CHECK(united.rectangle.width == 150);
    CHECK(united.rectangle.height == 100);
    CHECK(project.validate().empty());

    const auto outer = ice::addRectangle(project, cellId, layerId, {0, 0, 100, 100});
    const auto inner = ice::addRectangle(project, cellId, layerId, {25, 25, 50, 50});
    const auto differenceIds =
        ice::booleanLayoutShapes(project, cellId, {outer, inner}, ice::BooleanOperation::difference);
    CHECK(differenceIds.size() > 1);
    const auto &differenceCell = project.layoutCells.at(cellId);
    long double area = 0.0L;
    for (const auto id : differenceIds) {
        const auto &shape = differenceCell.shapes.at(id);
        CHECK(!shape.polygon.empty());
        ice::Polygon polygon;
        polygon.reserve(shape.polygon.size());
        for (const auto &point : shape.polygon)
            polygon.push_back({point.x, point.y});
        area += std::abs(polygonArea(polygon));
    }
    CHECK(area == 7500.0L);
    CHECK(differenceCell.shapes.size() == 1 + differenceIds.size());
    CHECK(project.validate().empty());
}

TEST_CASE("layout shape boolean operations reject mixed layers and empty results") {
    ice::Project project;
    const auto firstLayer = project.nextId();
    project.layers.emplace(firstLayer, ice::Layer{firstLayer, "metal1", 1});
    const auto secondLayer = project.nextId();
    project.layers.emplace(secondLayer, ice::Layer{secondLayer, "metal2", 2});
    const auto cellId = ice::createLayoutCell(project, "top");
    const auto first = ice::addRectangle(project, cellId, firstLayer, {0, 0, 100, 100});
    const auto second = ice::addRectangle(project, cellId, secondLayer, {50, 0, 100, 100});
    CHECK_THROWS_AS(ice::booleanLayoutShapes(project, cellId, {first, second}, ice::BooleanOperation::union_),
                    ice::ProjectError);
    CHECK(project.layoutCells.at(cellId).shapes.size() == 2);

    const auto third = ice::addRectangle(project, cellId, secondLayer, {500, 500, 10, 10});
    CHECK_THROWS_AS(ice::booleanLayoutShapes(project, cellId, {second, third}, ice::BooleanOperation::intersection),
                    ice::ProjectError);
    CHECK(project.layoutCells.at(cellId).shapes.size() == 3);
}

TEST_CASE("project loading can quarantine invalid sidecar files") {
    const auto root = std::filesystem::temp_directory_path() / "ice-project-recovery-test";
    std::filesystem::remove_all(root);
    ice::Project project;
    project.identity = "recovery-demo";
    project.name = "recovery-demo";
    ice::addLayer(project, "Metal1", 1);
    ice::createSchematicCell(project, "good");
    project.save(root);

    const auto badFile = root / "sch" / "bad.icesch";
    std::ofstream(badFile) << R"({"type":"text","pdk":"generic","text":"* hand-written"})";

    bool strictFailed = false;
    try {
        (void)ice::Project::load(root);
    } catch (const ice::ProjectError &error) {
        strictFailed = true;
        CHECK(std::string(error.what()).find("bad.icesch") != std::string::npos);
        CHECK(std::string(error.what()).find("project-repair") != std::string::npos);
    }
    CHECK(strictFailed);

    ice::ProjectLoadReport report;
    const auto recovered = ice::Project::load(root, ice::ProjectLoadOptions{true}, &report);
    CHECK(recovered.schematicCells.size() == 1);
    CHECK(recovered.schematicCells.begin()->second.name == "good");
    CHECK(report.quarantinedFiles.size() == 1);
    CHECK(std::filesystem::exists(report.quarantinedFiles.front()));
    CHECK(!std::filesystem::exists(badFile));
    CHECK(!report.warnings.empty());

    const auto loadedAgain = ice::Project::load(root);
    CHECK(loadedAgain.schematicCells.size() == 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("build info exposes a feature fingerprint") {
    const auto info = ice::buildInfo();
    CHECK(info.version == ice::version());
    CHECK(!info.commit.empty());
    CHECK(!info.buildTime.empty());
    CHECK(std::find(info.features.begin(), info.features.end(), "project_repair") != info.features.end());
    CHECK(std::find(info.features.begin(), info.features.end(), "simulation_metrics") != info.features.end());
    CHECK(std::find(info.features.begin(), info.features.end(), "pdk_status") != info.features.end());
    CHECK(std::find(info.features.begin(), info.features.end(), "implementation_runs") != info.features.end());
    CHECK(std::find(info.features.begin(), info.features.end(), "implementation_openroad_plan") != info.features.end());
}

TEST_CASE("generated AGENTS guidance is versioned and stable") {
    const auto root = std::filesystem::temp_directory_path() / "ice-agents-migration-test";
    std::filesystem::remove_all(root);
    ice::Project project;
    project.identity = "agents-migration";
    project.name = "agents-migration";
    project.save(root);

    const auto path = root / "AGENTS.md";
    auto read = [&] {
        std::ifstream input(path);
        return std::string((std::istreambuf_iterator<char>(input)), {});
    };
    const auto first = read();
    CHECK(first.find("ICE generated guidance: begin (version 5)") != std::string::npos);
    CHECK(first.find("simulation_measure") != std::string::npos);
    CHECK(first.find("pdk_info") != std::string::npos);
    CHECK(first.find("ask the user which installed PDK to use") != std::string::npos);
    CHECK(first.find("project_migrate") != std::string::npos);
    CHECK(first.find("zero crossings") != std::string::npos);
    CHECK(first.find("implementation_export_gds") != std::string::npos);
    CHECK(first.find("drc_run") != std::string::npos);

    project.save(root);
    CHECK(read() == first);

    std::ofstream(path) << "# Old project\n\nOld guidance.\n\nUser note.\n";
    project.save(root);
    const auto migrated = read();
    CHECK(migrated.find("Old guidance.") != std::string::npos);
    CHECK(migrated.find("User note.") != std::string::npos);
    CHECK(migrated.find("ICE generated guidance: begin") != std::string::npos);
    CHECK(migrated.find("simulation_measure") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("project saves reject stale revisions") {
    const auto root = std::filesystem::temp_directory_path() / "ice-revision-test";
    std::filesystem::remove_all(root);
    ice::Project project;
    project.identity = "revision-test";
    project.name = "revision-test";
    project.save(root);

    auto first = ice::Project::load(root);
    auto second = ice::Project::load(root);
    first.name = "first";
    first.save(root);
    second.name = "second";
    CHECK_THROWS_AS(second.save(root), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("PDK discovery reports missing OSDI models and a usable lib line") {
    const auto root = std::filesystem::temp_directory_path() / "ice-pdk-package-test";
    std::filesystem::remove_all(root);
    const auto models = root / "libs.tech" / "ngspice" / "models";
    std::filesystem::create_directories(models);
    std::ofstream(models / "cornerMOSlv.lib") << ".LIB mos_tt\n.model sg13_lv_nmos psp103va\n.ENDL mos_tt\n";
    std::ofstream(models / "sg13g2_moslv_parm.lib") << ".model sg13g2_lv_nmos_psp psp103va\n";
    std::ofstream(root / "package.toml") << R"([package]
schema_version = 1
name = "test.ihp"
version = "1.0.0"
license = "Apache-2.0"

[pdk]
id = "ihp-sg13g2"
display_name = "Test IHP"

[pdk.simulation]
engine = "ngspice"
root = "libs.tech/ngspice"
model_file = "models/cornerMOSlv.lib"
section = "mos_tt"
requires_osdi = true
verilog_a = "verilog-a"

[pdk.simulation.devices]
nmos = "sg13_lv_nmos"
pmos = "sg13_lv_pmos"
)";

    const auto pdks = ice::discoverPdks({root});
    CHECK(pdks.size() == 1);
    if (pdks.size() == 1) {
        CHECK(pdks.front().id == "ihp-sg13g2");
        CHECK(pdks.front().packageName == "test.ihp");
        CHECK(pdks.front().packageVersion == "1.0.0");
        CHECK(pdks.front().needsOsdi);
        CHECK(!pdks.front().simulationReady);
        CHECK(std::find(pdks.front().sections.begin(), pdks.front().sections.end(), "mos_tt") !=
              pdks.front().sections.end());
        CHECK(pdks.front().recommendedLibLine.find("cornerMOSlv.lib") != std::string::npos);
        CHECK(pdks.front().recommendedLibLine.find("mos_tt") != std::string::npos);
        CHECK(std::any_of(pdks.front().missing.begin(), pdks.front().missing.end(),
                          [](const std::string &value) { return value.find("OSDI") != std::string::npos; }));
        const auto json = ice::pdkInfoJson(pdks.front(), false);
        CHECK(json.at("simulation_ready") == false);
        CHECK(json.at("source") == "ice-package");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("simulation measurements count crossings and frequency") {
    const ice::SimulationTrace trace{"out", {0.0, 1.0, 0.0, 1.0, 0.0}};
    const std::vector<double> time{0.0, 1.0, 2.0, 3.0, 4.0};
    const auto measurement = ice::measureSignal(trace, time, 0.5);
    CHECK(measurement.statistics.points == 5);
    CHECK(measurement.statistics.min == 0.0);
    CHECK(measurement.statistics.max == 1.0);
    CHECK(measurement.zeroCrossings == 4);
    CHECK(measurement.risingCrossings == 2);
    CHECK(measurement.period.has_value());
    if (measurement.period)
        CHECK(std::abs(*measurement.period - 2.0) < 1e-12);
    CHECK(measurement.frequency.has_value());
    if (measurement.frequency)
        CHECK(std::abs(*measurement.frequency - 0.5) < 1e-12);
}

TEST_CASE("layout polygons ports and instances use core operations") {
    ice::Project project;
    project.identity = "layout-ops";
    project.name = "layout-ops";
    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto parent = ice::createLayoutCell(project, "parent");
    const auto child = ice::createLayoutCell(project, "child");
    const auto polygon = ice::addPolygon(project, parent, layer, {{0, 0}, {1000, 0}, {500, 800}});
    const auto port = ice::addPort(project, parent, "P1", layer, {0, 0, 100, 100});
    const auto instance = ice::addLayoutInstance(project, parent, child, "child_1", 200, 300);
    CHECK(project.layoutCells.at(parent).shapes.contains(polygon));
    CHECK(project.layoutCells.at(parent).ports.contains(port));
    CHECK(project.layoutCells.at(parent).instances.contains(instance));
    CHECK(project.layoutCells.at(parent).shapes.at(polygon).polygon.size() == 3);
    CHECK(project.validate().empty());
    const auto serialized = project.serialize();
    CHECK(ice::Project::deserialize(serialized).serialize() == serialized);
}

TEST_CASE("layout spacing changes do not reject off-grid layout or schematic data") {
    ice::Project project;
    project.identity = "layout-spacing-test";
    project.name = "layout-spacing-test";
    project.grid = 10;

    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto layout = ice::createLayoutCell(project, "top");
    ice::addPolygon(project, layout, layer, {{0, 0}, {13, 0}, {5, 17}});

    const auto schematic = ice::createSchematicCell(project, "top", "example.pdk");
    const auto symbolId = project.nextId();
    ice::Symbol symbol;
    symbol.id = symbolId;
    symbol.name = "device";
    symbol.artwork = "device";
    project.schematicCells.at(schematic).symbols.emplace(symbolId, symbol);
    const auto instanceId = project.nextId();
    project.schematicCells.at(schematic).instances.emplace(instanceId,
                                                           ice::SchematicInstance{instanceId, symbolId, "X1", 10, 10});

    ice::ProjectSession session(project);
    bool changed = true;
    try {
        session.execute("change layout spacing", [](ice::Project &value) { value.grid = 7; });
    } catch (...) {
        changed = false;
    }
    CHECK(changed);
    CHECK(session.project().grid == 7);
}

TEST_CASE("planned devices are projected from a schematic") {
    ice::Project project;
    project.identity = "planned";
    project.name = "planned";
    const auto schematicId = ice::createSchematicCell(project, "amp", "example.pdk");

    const auto resistorId = ice::addSymbol(project, schematicId, "resistor");
    auto &resistor = project.schematicCells.at(schematicId).symbols.at(resistorId);
    const auto resistorLeft = project.nextId();
    const auto resistorRight = project.nextId();
    resistor.pins.push_back({resistorLeft, "A", -10, 0, ice::PinDirection::left});
    resistor.pins.push_back({resistorRight, "B", 10, 0, ice::PinDirection::right});

    const auto capacitorId = ice::addSymbol(project, schematicId, "capacitor");
    auto &capacitor = project.schematicCells.at(schematicId).symbols.at(capacitorId);
    const auto capacitorLeft = project.nextId();
    const auto capacitorRight = project.nextId();
    capacitor.pins.push_back({capacitorLeft, "A", -10, 0, ice::PinDirection::left});
    capacitor.pins.push_back({capacitorRight, "B", 10, 0, ice::PinDirection::right});

    ice::addSchematicInstance(project, schematicId, resistorId, "R1", 0, 0);
    ice::addSchematicInstance(project, schematicId, capacitorId, "C1", 100, 0);
    const auto netId = project.nextId();
    project.schematicCells.at(schematicId).nets.emplace(netId, ice::Net{netId, "out", {resistorRight, capacitorLeft}});
    project.schematicCells.at(schematicId).nets.at(netId).labelAnchor =
        ice::NetLabelAnchor{resistorRight, capacitorLeft, 0.25, true};
    CHECK(project.validate().empty());

    const auto layoutId = ice::createLayoutFromSchematic(project, schematicId);
    const auto &layout = project.layoutCells.at(layoutId);
    CHECK(layout.pdk == "example.pdk");
    CHECK(layout.name == "amp");
    CHECK(layout.plannedDevices.size() == 2);
    CHECK(layout.plannedConnections.size() == 1);
    const auto &connection = layout.plannedConnections.begin()->second;
    const auto layoutNet = layout.nets.find(connection.net);
    CHECK(layoutNet != layout.nets.end());
    CHECK(layoutNet->second.name == "out");

    ice::ObjectId resistorDevice = 0;
    ice::ObjectId capacitorDevice = 0;
    for (const auto &[_, device] : layout.plannedDevices) {
        CHECK(device.width > 0);
        CHECK(device.height > 0);
        CHECK(device.x % project.grid == 0);
        CHECK(device.y % project.grid == 0);
        if (device.name == "R1")
            resistorDevice = device.id;
        if (device.name == "C1")
            capacitorDevice = device.id;
    }
    CHECK(resistorDevice != 0);
    CHECK(capacitorDevice != 0);
    CHECK(connection.from == resistorDevice);
    CHECK(connection.to == capacitorDevice);
    // The schematic placed C1 100 units to the right of R1, so the layout
    // boxes keep the same relative arrangement.
    CHECK(layout.plannedDevices.at(capacitorDevice).x - layout.plannedDevices.at(resistorDevice).x == 1000);
    CHECK(layout.plannedDevices.at(capacitorDevice).y == layout.plannedDevices.at(resistorDevice).y);
    CHECK(project.validate().empty());

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.layoutCells.at(layoutId).plannedDevices.size() == 2);
    CHECK(roundTrip.layoutCells.at(layoutId).plannedConnections.size() == 1);
    CHECK(roundTrip.layoutCells.at(layoutId).plannedDevices.at(resistorDevice).name == "R1");
    const auto roundTripConnection = roundTrip.layoutCells.at(layoutId).plannedConnections.begin()->second;
    const auto roundTripNet = roundTrip.layoutCells.at(layoutId).nets.find(roundTripConnection.net);
    CHECK(roundTripNet != roundTrip.layoutCells.at(layoutId).nets.end());
    CHECK(roundTripNet->second.name == "out");
    const auto &roundTripSchematicNet = roundTrip.schematicCells.at(schematicId).nets.at(netId);
    CHECK(roundTripSchematicNet.labelAnchor.has_value());
    CHECK(roundTripSchematicNet.labelAnchor->fromPin == resistorRight);
    CHECK(roundTripSchematicNet.labelAnchor->toPin == capacitorLeft);
    CHECK(roundTripSchematicNet.labelAnchor->fraction == 0.25);
    CHECK(roundTripSchematicNet.labelAnchor->sideB);

    const auto secondLayoutId = ice::createLayoutFromSchematic(project, schematicId);
    CHECK(project.layoutCells.at(secondLayoutId).name != layout.name);
}

TEST_CASE("planned connections can target placed layout instance pins") {
    ice::Project project;
    project.identity = "planned-instance";
    project.name = "planned-instance";
    const auto schematicId = ice::createSchematicCell(project, "amp", "example.pdk");

    const auto symbolId = ice::addSymbol(project, schematicId, "NMOS");
    auto &symbol = project.schematicCells.at(schematicId).symbols.at(symbolId);
    const auto drain = project.nextId();
    symbol.pins.push_back({drain, "D", 0, 0, ice::PinDirection::right});
    const auto schematicInstance = ice::addSchematicInstance(project, schematicId, symbolId, "M1", 0, 0);

    const auto layoutId = ice::createLayoutFromSchematic(project, schematicId);
    auto &layout = project.layoutCells.at(layoutId);
    CHECK(layout.plannedDevices.size() == 1);
    const auto plannedId = layout.plannedDevices.begin()->first;

    const auto childId = ice::createLayoutCell(project, "child", "example.pdk");
    const auto layerId = ice::addLayer(project, "Metal1", 8);
    const auto portId = ice::addPort(project, childId, "D", layerId, ice::Rect{0, 0, 100, 200});
    const auto instanceId = project.nextId();
    ice::LayoutInstance instance{instanceId, childId, "X1", 100, 100, ice::Orientation::r0};
    instance.schematicInstance = schematicInstance;
    layout.instances.emplace(instanceId, instance);

    const auto connectionId = project.nextId();
    ice::PlannedConnection connection{connectionId, plannedId, instanceId, 0};
    connection.fromPin = "D";
    connection.toPin = "D";
    connection.toPort = portId;
    connection.fromSide = ice::PortSide::right;
    connection.toSide = ice::PortSide::top;
    connection.routed = true;
    layout.plannedConnections.emplace(connectionId, connection);
    const auto topPortId = ice::addPort(project, layoutId, "OUT", layerId, ice::Rect{200, 200, 100, 100});
    layout.ports.at(topPortId).schematicInstance = schematicInstance;
    const auto topConnectionId = project.nextId();
    ice::PlannedConnection topConnection{topConnectionId, plannedId, topPortId, 0};
    topConnection.fromPin = "D";
    layout.plannedConnections.emplace(topConnectionId, topConnection);
    CHECK(project.validate().empty());
    CHECK(ice::schematicForLayout(project, layoutId) == schematicId);

    auto badConnection = connection;
    badConnection.id = project.nextId();
    badConnection.toPort = project.nextId();
    layout.plannedConnections.emplace(badConnection.id, badConnection);
    CHECK(!project.validate().empty());
    layout.plannedConnections.erase(badConnection.id);

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    const auto &roundLayout = roundTrip.layoutCells.at(layoutId);
    CHECK(roundLayout.instances.contains(instanceId));
    CHECK(roundLayout.instances.at(instanceId).schematicInstance == schematicInstance);
    CHECK(roundLayout.plannedConnections.contains(connectionId));
    const auto &roundConnection = roundLayout.plannedConnections.at(connectionId);
    CHECK(roundConnection.fromPin == "D");
    CHECK(roundConnection.toPin == "D");
    CHECK(roundConnection.toPort == portId);
    CHECK(roundConnection.fromSide == ice::PortSide::right);
    CHECK(roundConnection.toSide == ice::PortSide::top);
    CHECK(roundConnection.routed);
    CHECK(roundLayout.plannedConnections.contains(topConnectionId));
    CHECK(roundLayout.plannedConnections.at(topConnectionId).to == topPortId);
    CHECK(roundLayout.ports.contains(topPortId));
    CHECK(roundLayout.ports.at(topPortId).schematicInstance == schematicInstance);
    CHECK(roundTrip.validate().empty());
}

TEST_CASE("interchange, verification, and plugin lifecycle are explicit") {
    ice::Project project;
    project.identity = "design";
    project.name = "design";
    auto layer = project.nextId();
    project.layers.emplace(layer, ice::Layer{layer, "metal1", 1});
    auto cell = project.nextId();
    ice::LayoutCell layout{cell, "top"};
    auto shape = project.nextId();
    layout.shapes.emplace(shape, ice::Shape{shape, layer, std::nullopt, {0, 0, 10, 10}});
    project.layoutCells.emplace(cell, layout);
    auto path = std::filesystem::temp_directory_path() / "ice-layout.gds";
    ice::LayoutInterchange::exportGds(project, cell, path);
    auto imported = ice::LayoutInterchange::importGds(path);
    CHECK(imported.layoutCells.size() == 1);
    auto report = ice::LocalVerificationAdapter().run(project);
    CHECK(report.drc.empty());
    CHECK(ice::serializeVerificationReport(report).find("\"pex\"") != std::string::npos);
    auto future = ice::VerificationScheduler().schedule(project, std::make_shared<ice::LocalVerificationAdapter>());
    CHECK(future.get().schemaVersion == 1);
    std::filesystem::remove(path);

    auto pluginRoot = std::filesystem::temp_directory_path() / "ice-plugin";
    std::filesystem::create_directories(pluginRoot);
    std::ofstream(pluginRoot / "plugin.toml")
        << "[plugin]\nname=\"test-plugin\"\nversion=\"1.0.0\"\nice_target=\"0.1.0\"\n"
           "\n[permissions]\nread_project=true\nmutate_project=false\n";
    ice::PluginManager plugins;
    plugins.discover(pluginRoot);
    plugins.initialize("test-plugin");
    plugins.enable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::enabled);
    plugins.disable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::disabled);
    // Disabled plugins can be re-enabled without re-initialization.
    plugins.enable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::enabled);
    plugins.disable("test-plugin");
    plugins.unload("test-plugin");
    std::filesystem::remove_all(pluginRoot);
}

TEST_CASE("project reports are project-level metadata") {
    ice::Project project;
    project.identity = "design";
    project.name = "design";
    const auto reportId = project.nextId();
    project.reports.emplace(reportId, ice::ProjectReport{reportId, "rlc comparison", "scripts/rlc_comparison.py"});
    CHECK(project.validate().empty());
    CHECK(project.nextId() == reportId + 1);
    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.reports.contains(reportId));
    CHECK(roundTrip.reports.at(reportId).name == "rlc comparison");
    CHECK(roundTrip.reports.at(reportId).path == "scripts/rlc_comparison.py");

    auto invalid = project;
    invalid.reports.at(reportId).path = "../outside.py";
    CHECK(!invalid.validate().empty());
    auto invalidExtension = project;
    invalidExtension.reports.at(reportId).path = "scripts/outside.txt";
    CHECK(!invalidExtension.validate().empty());
}

TEST_CASE("simulation runs are persisted under the project build directory") {
    ice::Project project;
    project.identity = "design";
    project.name = "design";
    const auto simulationId = project.nextId();
    ice::SimulationItem simulation;
    simulation.id = simulationId;
    simulation.name = "rlc_sim";
    project.simulations.emplace(simulationId, simulation);

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.stop = 0.01;
    request.step = 1e-5;
    request.netlist = {"V1 in 0 5", "R1 in n1 1k"};
    request.probeSignals = {"v(in)", "v(n1)"};

    ice::SimulationResult result;
    result.state = ice::JobState::succeeded;
    result.analysis = "transient";
    result.simulator = "ngspice";
    result.simulator_version = "test";
    result.time = {0.0, 1e-6};
    result.traces = {{"v(in)", {5.0, 5.0}}, {"v(n1)", {0.0, 1.0}}};

    ice::SimulationRunContext context;
    context.projectRoot = std::filesystem::temp_directory_path() / "ice-simulation-store";
    context.projectIdentity = project.identity;
    context.projectName = project.name;
    context.simulationId = simulationId;
    context.simulationName = simulation.name;
    context.schematicId = 42;
    context.schematicName = "rlc";
    context.request = request;
    context.directives = "";
    context.netlistNotes = {"test note"};
    ice::SimulationGraph graph;
    graph.id = 1;
    graph.name = "RLC response";
    graph.ySignals = {"v(in)", "v(n1)"};
    context.graphs.push_back(graph);

    std::filesystem::remove_all(context.projectRoot);
    const auto serialized = ice::serializeSimulationRun(context, result);
    CHECK(serialized.find("\"format\": \"ice-simulation-result\"") != std::string::npos);
    CHECK(serialized.find("\"simulationName\"") == std::string::npos);
    CHECK(serialized.find("\"name\": \"rlc_sim\"") != std::string::npos);
    CHECK(serialized.find("\"netlist_notes\"") != std::string::npos);

    const auto artifact = ice::writeSimulationRun(context, result);
    CHECK(std::filesystem::is_regular_file(artifact.path));
    CHECK(!std::filesystem::exists(artifact.path.string() + ".tmp"));
    CHECK(artifact.path.string().find("build/simulations/" + std::to_string(simulationId) + "-rlc_sim/runs/") !=
          std::string::npos);
    CHECK(artifact.path.stem().string() == artifact.runId);
    std::ifstream input(artifact.path);
    const std::string content((std::istreambuf_iterator<char>(input)), {});
    CHECK(content.find("\"format\": \"ice-simulation-result\"") != std::string::npos);
    CHECK(content.find("\"state\": \"succeeded\"") != std::string::npos);
    CHECK(content.find("\"test note\"") != std::string::npos);
    CHECK(content.find("\"RLC response\"") != std::string::npos);

    const auto stored = ice::readSimulationRun(artifact.path);
    CHECK(stored.runId == artifact.runId);
    CHECK(stored.simulationId == simulationId);
    CHECK(stored.schematicId == 42);
    CHECK(stored.result.time.size() == 2);
    CHECK(stored.result.traces.size() == 2);
    const auto latest = ice::latestSimulationRun(context.projectRoot, simulationId);
    CHECK(latest.has_value());
    CHECK(latest->path == artifact.path);
    const auto latestBySchematic = ice::latestSimulationRun(context.projectRoot, std::nullopt, 42);
    CHECK(latestBySchematic.has_value());
    CHECK(latestBySchematic->runId == artifact.runId);

    auto failed = result;
    failed.state = ice::JobState::failed;
    failed.error = "simulated failure";
    failed.time.clear();
    failed.traces.clear();
    const auto failedArtifact = ice::writeSimulationRun(context, failed);
    std::ifstream failedInput(failedArtifact.path);
    const std::string failedContent((std::istreambuf_iterator<char>(failedInput)), {});
    CHECK(failedContent.find("\"state\": \"failed\"") != std::string::npos);
    CHECK(failedContent.find("simulated failure") != std::string::npos);

    auto traversal = context;
    traversal.simulationName = "../../escape";
    const auto directory = ice::simulationRunDirectory(context.projectRoot, traversal);
    CHECK(directory.string().find("..") == std::string::npos);
    CHECK(directory.string().find("-escape") != std::string::npos);
    std::filesystem::remove_all(context.projectRoot);
}

TEST_CASE("project reports can be created and edited through core helpers") {
    const auto root = std::filesystem::temp_directory_path() / "ice-report-core-test";
    std::filesystem::remove_all(root);

    ice::Project project;
    project.identity = "report-test";
    project.name = "report-test";
    const auto reportId = ice::createProjectReport(project, root, "Comparison Report",
                                                   "# custom report\ndef build_script(ctx):\n    return \"# ok\"\n");
    CHECK(project.reports.contains(reportId));
    const auto &report = project.reports.at(reportId);
    CHECK(report.name == "Comparison Report");
    CHECK(report.path == "scripts/comparison-report.py");
    CHECK(std::filesystem::is_regular_file(root / report.path));
    CHECK(ice::readProjectReportSource(root, report).find("# custom report") != std::string::npos);

    ice::writeProjectReportSource(root, report, "# updated\n");
    CHECK(ice::readProjectReportSource(root, report) == "# updated\n");
    CHECK(project.validate().empty());
    const auto graphPath = ice::projectSimulationScriptGraphPath(root, report);
    CHECK(graphPath.filename() == "graphs.json");
    CHECK(graphPath.string().find("build/simulation-scripts") != std::string::npos);
    CHECK(ice::sanitizedReportSlug("../../Comp/are") == "comp-are");
    CHECK_THROWS_AS(ice::createProjectReport(project, root, "Comparison Report", "# duplicate\n"), ice::ProjectError);

    const auto templateReportId = ice::createProjectReport(project, root, "Template Report");
    CHECK(ice::readProjectReportSource(root, project.reports.at(templateReportId)).find("# ICE simulation script") !=
          std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("simulation script graph payloads are parsed for the GUI") {
    const std::string payload = R"JSON({
  "format": "ice-simulation-script-graphs",
  "version": 1,
  "graphs": [
    {
      "name": "RLC response",
      "x_axis": "time",
      "x": [0.0, 1e-9, 2e-9],
      "series": [
        {"name": "in", "values": [0.0, 5.0, 5.0]},
        {"name": "n2", "values": [0.0, 0.2, 0.4]}
      ],
      "x_min": 0.0,
      "x_max": 2e-9,
      "y_min": null,
      "y_max": 5.0
    }
  ]
})JSON";
    const auto graphs = ice::parseSimulationScriptGraphs(payload);
    CHECK(graphs.size() == 1);
    CHECK(graphs[0].name == "RLC response");
    CHECK(graphs[0].x.size() == 3);
    CHECK(graphs[0].series.size() == 2);
    CHECK(graphs[0].series[1].name == "n2");
    CHECK(graphs[0].xMin.has_value());
    CHECK(!graphs[0].yMin.has_value());
    CHECK_THROWS_AS(ice::parseSimulationScriptGraphs("{\"format\":\"ice-simulation-script-graphs\","
                                                     "\"graphs\":[{\"name\":\"bad\",\"x\":[0,1],"
                                                     "\"series\":[{\"name\":\"s\",\"values\":[0]}]}]}"),
                    ice::ProjectError);
}

TEST_CASE("project items can be removed with reference checks") {
    ice::Project project;
    project.identity = "remove-test";
    project.name = "remove-test";

    const auto usedSchematic = ice::createSchematicCell(project, "used");
    const auto unusedSchematic = ice::createSchematicCell(project, "unused");
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "sim";
    simulation.schematic = usedSchematic;
    project.simulations.emplace(simulation.id, simulation);
    CHECK_THROWS_AS(ice::removeSchematicCell(project, usedSchematic), ice::ProjectError);
    ice::removeSimulationItem(project, simulation.id);
    CHECK(!project.simulations.contains(simulation.id));
    ice::removeSchematicCell(project, usedSchematic);
    ice::removeSchematicCell(project, unusedSchematic);
    CHECK(project.schematicCells.empty());

    const auto parentSchematic = ice::createSchematicCell(project, "parent");
    const auto childSchematic = ice::createSchematicCell(project, "child");
    ice::addSymbol(project, parentSchematic, "child_block", {}, childSchematic);
    CHECK_THROWS_AS(ice::removeSchematicCell(project, childSchematic), ice::ProjectError);

    const auto parentLayout = ice::createLayoutCell(project, "parent_layout");
    const auto childLayout = ice::createLayoutCell(project, "child_layout");
    ice::addLayoutInstance(project, parentLayout, childLayout, "I1", 0, 0);
    CHECK_THROWS_AS(ice::removeLayoutItem(project, childLayout), ice::ProjectError);
    ice::removeLayoutItem(project, parentLayout);
    ice::removeLayoutItem(project, childLayout);
    CHECK(project.layoutCells.empty());

    const auto libraryLayout = ice::createLayoutCell(project, "library_layout");
    project.layoutCells.at(libraryLayout).library = true;
    CHECK_THROWS_AS(ice::removeLayoutItem(project, libraryLayout), ice::ProjectError);

    const auto proceduralId = ice::createProceduralLayout(project, "generated");
    const auto generatedId = project.proceduralLayouts.at(proceduralId).generatedCell;
    CHECK_THROWS_AS(ice::removeLayoutItem(project, generatedId), ice::ProjectError);
    ice::removeLayoutItem(project, proceduralId);
    CHECK(!project.proceduralLayouts.contains(proceduralId));
    CHECK(!project.layoutCells.contains(generatedId));
}

TEST_CASE("layout paths persist and move as one shape") {
    ice::Project project;
    project.identity = "path-test";
    project.name = "path-test";
    const auto layer = ice::addLayer(project, "Metal1", 1);
    const auto secondLayer = ice::addLayer(project, "Metal2", 2);
    const auto viaLayer = ice::addLayer(project, "Via1", 3);
    const auto cell = ice::createLayoutCell(project, "top");
    const auto pathId = project.nextId();
    ice::Shape path{pathId, layer, std::nullopt, {0, -200, 1200, 400}, {}};
    path.centerline = {{0, 0}, {1000, 0}, {1000, 200}};
    path.segmentLayers = {layer, secondLayer};
    path.pointVias = {{}, {viaLayer}, {}};
    path.width = 300;
    path.segmentWidths = {200, 300};
    path.polygon = {{-100, -100}, {1100, -100}, {1100, 300}, {-100, 300}};
    project.layoutCells.at(cell).shapes.emplace(pathId, path);
    CHECK(project.validate().empty());

    auto roundTrip = ice::Project::deserialize(project.serialize());
    const auto &loaded = roundTrip.layoutCells.at(cell).shapes.at(pathId);
    CHECK(loaded.isPath());
    CHECK(loaded.width == 300);
    CHECK(loaded.centerline.size() == 3);
    CHECK(loaded.segmentLayers.size() == 2);
    CHECK(loaded.layerForSegment(1) == secondLayer);
    CHECK(loaded.segmentWidths.size() == 2);
    CHECK(loaded.widthForSegment(0) == 200);
    CHECK(loaded.widthForSegment(1) == 300);
    CHECK(loaded.pointVias.size() == 3);
    CHECK(loaded.pointVias[1].size() == 1);
    CHECK(loaded.pointVias[1].front() == viaLayer);

    ice::moveRectangle(roundTrip, cell, pathId, 50, 25);
    const auto &moved = roundTrip.layoutCells.at(cell).shapes.at(pathId);
    CHECK(moved.centerline.front().x == 50);
    CHECK(moved.centerline.front().y == 25);
    CHECK(moved.polygon.front().x == -50);
    CHECK(moved.polygon.front().y == -75);

    const auto gds = std::filesystem::temp_directory_path() / "ice-layout-path-test.gds";
    ice::LayoutInterchange::exportGds(project, cell, gds);
    CHECK(std::filesystem::is_regular_file(gds));
    const auto imported = ice::LayoutInterchange::importGds(gds);
    const auto &importedCell = imported.layoutCells.begin()->second;
    bool metal1ReachesVia = false;
    bool metal2ReachesVia = false;
    bool viaExported = false;
    for (const auto &[_, shape] : importedCell.shapes) {
        const auto importedLayer = imported.layers.find(shape.layer);
        if (importedLayer == imported.layers.end())
            continue;
        if (importedLayer->second.number == 1)
            metal1ReachesVia = shape.rectangle.x + shape.rectangle.width == 1150;
        else if (importedLayer->second.number == 2)
            metal2ReachesVia = shape.rectangle.y == -150;
        else if (importedLayer->second.number == 3)
            viaExported = true;
    }
    CHECK(metal1ReachesVia);
    CHECK(metal2ReachesVia);
    CHECK(viaExported);
    std::filesystem::remove(gds);
}

TEST_CASE("layout text persists and exports as GDS text") {
    ice::Project project;
    project.identity = "text-test";
    project.name = "text-test";
    const auto layer = ice::addLayer(project, "Metal1", 1);
    const auto cell = ice::createLayoutCell(project, "top");
    const auto textId = ice::addText(project, cell, layer, 1000, 2000, "VDD", 500);
    const auto &text = project.layoutCells.at(cell).shapes.at(textId);
    CHECK(text.isText());
    CHECK(text.name == "Text 1");
    CHECK(text.text == "VDD");
    CHECK(text.rectangle.x == 1000);
    CHECK(text.rectangle.y == 2000);
    CHECK(text.rectangle.height == 500);
    CHECK(project.validate().empty());
    CHECK_THROWS_AS(ice::addText(project, cell, layer, 0, 0, "", 500), ice::ProjectError);

    auto roundTrip = ice::Project::deserialize(project.serialize());
    const auto &loaded = roundTrip.layoutCells.at(cell).shapes.at(textId);
    CHECK(loaded.isText());
    CHECK(loaded.name == "Text 1");
    CHECK(loaded.text == "VDD");

    const auto gds = std::filesystem::temp_directory_path() / "ice-layout-text-test.gds";
    ice::LayoutInterchange::exportGds(project, cell, gds);
    CHECK(std::filesystem::is_regular_file(gds));
    const auto imported = ice::LayoutInterchange::importGds(gds);
    bool foundText = false;
    for (const auto &[_, shape] : imported.layoutCells.begin()->second.shapes) {
        if (shape.isText() && shape.text == "VDD") {
            foundText = true;
            CHECK(shape.rectangle.height == 1000);
        }
    }
    CHECK(foundText);
    std::filesystem::remove(gds);
}

TEST_CASE("layout polygon points can be moved") {
    ice::Project project;
    project.identity = "polygon-edit-test";
    project.name = "polygon-edit-test";
    const auto layer = ice::addLayer(project, "Metal1", 1);
    const auto cell = ice::createLayoutCell(project, "top");
    const auto polygonId = ice::addPolygon(project, cell, layer, {{0, 0}, {1000, 0}, {500, 800}});
    ice::movePolygonPoint(project, cell, polygonId, 2, {600, 900});
    const auto &shape = project.layoutCells.at(cell).shapes.at(polygonId);
    CHECK(shape.name == "Polygon 1");
    CHECK(shape.polygon.at(2).x == 600);
    CHECK(shape.polygon.at(2).y == 900);
    CHECK(shape.rectangle.x == 0);
    CHECK(shape.rectangle.y == 0);
    CHECK(shape.rectangle.width == 1000);
    CHECK(shape.rectangle.height == 900);
    CHECK_THROWS_AS(ice::movePolygonPoint(project, cell, polygonId, 3, {0, 0}), ice::ProjectError);
}

TEST_CASE("simulation graph metadata is editable through core helpers") {
    ice::Project project;
    project.identity = "graph-test";
    project.name = "graph-test";
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "tran1";
    project.simulations.emplace(simulation.id, simulation);

    ice::SimulationGraph graph;
    graph.name = "RLC response";
    graph.xAxis = "time";
    graph.ySignals = {"v(n1)", "v(n2)"};
    const auto graphId = ice::addSimulationGraph(project, simulation.id, graph);
    CHECK(project.simulations.at(simulation.id).graphs.size() == 1);
    CHECK(project.simulations.at(simulation.id).graphs.front().id == graphId);
    CHECK(graphId > simulation.id);

    auto updated = project.simulations.at(simulation.id).graphs.front();
    updated.name = "Outputs";
    updated.xAxis = "v(n1)";
    updated.ySignals = {"v(n2)"};
    updated.xMin = 0.0;
    updated.xMax = 1e-3;
    updated.yMax = 6.0;
    CHECK(ice::updateSimulationGraph(project, simulation.id, graphId, updated));
    const auto &stored = project.simulations.at(simulation.id).graphs.front();
    CHECK(stored.name == "Outputs");
    CHECK(stored.xAxis == "v(n1)");
    CHECK(stored.ySignals == std::vector<std::string>{"v(n2)"});
    CHECK(stored.xMin.has_value());
    CHECK(stored.xMax.has_value());
    CHECK(!stored.yMin.has_value());
    CHECK(stored.yMax.has_value());

    CHECK(ice::removeSimulationGraph(project, simulation.id, graphId));
    CHECK(project.simulations.at(simulation.id).graphs.empty());
    CHECK(!ice::removeSimulationGraph(project, simulation.id, graphId));
    CHECK_THROWS_AS(ice::addSimulationGraph(project, 9999, graph), ice::ProjectError);
}

TEST_CASE("schematic comments survive serialization") {
    ice::Project project;
    project.identity = "comment-test";
    project.name = "comment-test";
    const auto cellId = ice::createSchematicCell(project, "top");
    const auto commentId = ice::addSchematicComment(project, cellId, "Test schematic note", 120, -80);

    const auto json = project.serialize();
    const auto loaded = ice::Project::deserialize(json);
    const auto &cell = loaded.schematicCells.at(cellId);
    CHECK(cell.comments.size() == 1);
    const auto it = cell.comments.find(commentId);
    CHECK(it != cell.comments.end());
    if (it != cell.comments.end()) {
        CHECK(it->second.text == "Test schematic note");
        CHECK(it->second.x == 120);
        CHECK(it->second.y == -80);
    }

    ice::removeSchematicComment(project, cellId, commentId);
    CHECK(project.schematicCells.at(cellId).comments.empty());
}
