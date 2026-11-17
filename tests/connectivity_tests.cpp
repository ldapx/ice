#include "icelib/connectivity.hpp"
#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct ConnectivityFixture {
    ice::Project project;
    ice::ObjectId cell = 0;
    ice::ObjectId metal = 0;
    ice::ObjectId via = 0;
};

ConnectivityFixture makeFixture() {
    ConnectivityFixture fixture;
    fixture.project.identity = "connectivity";
    fixture.project.name = "connectivity";
    fixture.cell = ice::createLayoutCell(fixture.project, "top");
    fixture.metal = ice::addLayer(fixture.project, "Metal1", 10);
    fixture.via = ice::addLayer(fixture.project, "Via1", 20);
    return fixture;
}

const ice::LayoutNetGroup *groupFor(const std::vector<ice::LayoutNetGroup> &groups, ice::ObjectId shape) {
    const auto found = std::find_if(groups.begin(), groups.end(), [shape](const ice::LayoutNetGroup &group) {
        return std::find(group.shapes.begin(), group.shapes.end(), shape) != group.shapes.end();
    });
    return found == groups.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE("geometric connectivity merges same-layer and via-connected shapes") {
    auto fixture = makeFixture();
    const auto left = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {0, 0, 1000, 1000});
    const auto overlapping = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {500, 500, 1000, 1000});
    const auto isolated = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {5000, 0, 500, 500});
    const auto via = ice::addRectangle(fixture.project, fixture.cell, fixture.via, {2400, 0, 200, 200});
    const auto upperMetal = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {2300, -100, 600, 400});

    const auto groups = ice::extractLayoutNets(fixture.project, fixture.cell);
    const auto *lower = groupFor(groups, left);
    const auto *upper = groupFor(groups, upperMetal);
    CHECK(lower != nullptr);
    CHECK(upper != nullptr);
    CHECK(lower == groupFor(groups, overlapping));
    CHECK(upper == groupFor(groups, via));
    CHECK(lower != upper);
    CHECK(groupFor(groups, isolated) != lower);
    CHECK(groupFor(groups, isolated) != upper);
}

TEST_CASE("via cuts only connect their own layer pair") {
    ice::Project project;
    project.identity = "via-endpoints";
    project.name = "via-endpoints";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto li1 = ice::addLayer(project, "li1", 67, 0, {}, 20);
    const auto met1 = ice::addLayer(project, "met1", 68, 0, {}, 20);
    const auto mcon = ice::addLayer(project, "mcon", 67, 0, {}, 44);
    const auto met2 = ice::addLayer(project, "met2", 69, 0, {}, 20);
    const auto via = ice::addLayer(project, "via", 68, 0, {}, 44);

    const auto lower = ice::addRectangle(project, cell, met1, {0, 0, 1000, 1000});
    const auto landing = ice::addRectangle(project, cell, li1, {0, 0, 1000, 1000});
    const auto mconCut = ice::addRectangle(project, cell, mcon, {400, 400, 200, 200});
    const auto upper = ice::addRectangle(project, cell, met2, {0, 0, 1000, 1000});

    auto groups = ice::extractLayoutNets(project, cell);
    CHECK(groupFor(groups, lower) != nullptr);
    CHECK(groupFor(groups, lower) == groupFor(groups, landing));
    CHECK(groupFor(groups, lower) == groupFor(groups, mconCut));
    CHECK(groupFor(groups, lower) != groupFor(groups, upper));

    const auto viaCut = ice::addRectangle(project, cell, via, {600, 600, 200, 200});
    groups = ice::extractLayoutNets(project, cell);
    CHECK(groupFor(groups, lower) == groupFor(groups, upper));
    CHECK(groupFor(groups, lower) == groupFor(groups, viaCut));
}

TEST_CASE("explicit layout nets merge distant shapes and pins") {
    auto fixture = makeFixture();
    const auto net = ice::createLayoutNet(fixture.project, fixture.cell, "VDD");
    const auto shape = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {0, 0, 500, 500}, net);
    const auto pin = ice::addPort(fixture.project, fixture.cell, "VDD", fixture.metal, {5000, 0, 500, 500},
                                  ice::PortDirection::power, ice::PortSide::left, net, "VDD");

    const auto groups = ice::extractLayoutNets(fixture.project, fixture.cell);
    CHECK(groups.size() == 1);
    if (groups.size() != 1)
        return;
    CHECK(groups.front().net == net);
    CHECK(groups.front().name == "VDD");
    CHECK(groups.front().shapes == std::vector<ice::ObjectId>{shape});
    CHECK(groups.front().ports == std::vector<ice::ObjectId>{pin});
}

TEST_CASE("distinct explicit nets stay separate when geometry overlaps") {
    auto fixture = makeFixture();
    const auto powerNet = ice::createLayoutNet(fixture.project, fixture.cell, "VDD");
    const auto groundNet = ice::createLayoutNet(fixture.project, fixture.cell, "VSS");
    const auto power = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {0, 0, 1000, 1000}, powerNet);
    const auto ground =
        ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {500, 500, 1000, 1000}, groundNet);

    const auto groups = ice::extractLayoutNets(fixture.project, fixture.cell);
    CHECK(groups.size() == 2);
    CHECK(groupFor(groups, power) != groupFor(groups, ground));
}

TEST_CASE("connectivity ignores hidden pins and text shapes") {
    auto fixture = makeFixture();
    const auto shape = ice::addRectangle(fixture.project, fixture.cell, fixture.metal, {0, 0, 1000, 1000});
    const auto hidden = ice::addPort(fixture.project, fixture.cell, "HIDDEN", fixture.metal, {0, 0, 1000, 1000});
    auto &port = fixture.project.layoutCells.at(fixture.cell).ports.at(hidden);
    port.visible = false;
    ice::addText(fixture.project, fixture.cell, fixture.metal, 100, 100, "NOTE");

    const auto groups = ice::extractLayoutNets(fixture.project, fixture.cell);
    CHECK(groups.size() == 1);
    if (groups.size() != 1)
        return;
    CHECK(groups.front().shapes == std::vector<ice::ObjectId>{shape});
    CHECK(groups.front().ports.empty());
}

TEST_CASE("package connectivity metadata declares roles and via endpoints") {
    const auto root = std::filesystem::temp_directory_path() / "ice-pdk-connectivity-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "technology");
    std::ofstream(root / "technology" / "connectivity.json") << R"({
  "format": "ice-pdk-connectivity",
  "schema_version": 1,
  "layers": [
    {"name": "diff", "role": "active"},
    {"name": "poly", "role": "gate"},
    {"name": "met1", "role": "conductor", "sheet_resistance": 0.125},
    {"name": "met2", "role": "conductor"},
    {"name": "via", "role": "via"},
    {"name": "nwell", "role": "marker"}
  ],
  "vias": [
    {"name": "via", "lower": ["met1"], "upper": ["met2"], "size": 150, "enclosure": 60}
  ],
  "routing": {"stack": ["met1", "met2"]}
})";
    const auto connectivity = ice::PdkConnectivity::load(root / "technology" / "connectivity.json");
    CHECK(connectivity.declared);
    CHECK(connectivity.roleFor("met1.drawing") == "conductor");
    CHECK(connectivity.roleFor("met1.pin") == "conductor");
    CHECK(connectivity.roleFor("poly.drawing") == "gate");
    CHECK(connectivity.roleFor("nwell") == "marker");
    CHECK(connectivity.roleFor("met1.label") == "marker");
    CHECK(connectivity.roleFor("met1.pin") == "conductor");
    CHECK(connectivity.roleFor("unknown") == "");
    CHECK(connectivity.viaFor("via.drawing") != nullptr);
    if (const auto *via = connectivity.viaFor("via.drawing")) {
        CHECK(via->lower == std::vector<std::string>{"met1"});
        CHECK(via->upper == std::vector<std::string>{"met2"});
        CHECK(via->size == 150.0);
        CHECK(via->enclosure == 60.0);
    }
    CHECK(connectivity.viaBetween("met2", "met1") != nullptr);
    CHECK(connectivity.viaBetween("met1", "poly") == nullptr);
    std::filesystem::remove_all(root);
}

TEST_CASE("declared package connectivity drives the layer graph") {
    const auto root = std::filesystem::temp_directory_path() / "ice-pdk-connectivity-graph";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "technology");
    std::ofstream(root / "package.toml") << R"([package]
schema_version = 1
name = "test.connectivity"
version = "1.0.0"
license = "Apache-2.0"

[pdk]
id = "connectivity-test"
display_name = "Connectivity Test"
)";
    std::ofstream(root / "technology" / "connectivity.json") << R"({
  "schema_version": 1,
  "layers": [
    {"name": "diff", "role": "active"},
    {"name": "poly", "role": "gate"},
    {"name": "met1", "role": "conductor"},
    {"name": "met2", "role": "conductor"},
    {"name": "via", "role": "via"},
    {"name": "nwell", "role": "marker"}
  ],
  "vias": [
    {"name": "via", "lower": ["met1"], "upper": ["met2"], "size": 150, "enclosure": 60}
  ],
  "routing": {"stack": ["met1", "met2"]}
})";
    const auto previous = std::getenv("ICE_PACKAGE_ROOTS");
    const auto saved = previous ? std::string(previous) : std::string{};
    setenv("ICE_PACKAGE_ROOTS", root.c_str(), 1);

    ice::Project project;
    project.identity = "connectivity-graph";
    project.name = "connectivity-graph";
    (void)ice::createLayoutCell(project, "top", "connectivity-test");
    const auto diff = ice::addLayer(project, "diff.drawing", 65, 0, {}, 20);
    const auto poly = ice::addLayer(project, "poly.drawing", 66, 0, {}, 20);
    const auto met1 = ice::addLayer(project, "met1.drawing", 68, 0, {}, 20);
    const auto met2 = ice::addLayer(project, "met2.drawing", 69, 0, {}, 20);
    const auto via = ice::addLayer(project, "via.drawing", 68, 0, {}, 44);
    const auto nwell = ice::addLayer(project, "nwell.drawing", 64, 0, {}, 20);

    const auto graph = ice::inferLayoutConnectivity(project, "connectivity-test");
    CHECK(!graph.conductors.contains(diff));
    CHECK(!graph.conductors.contains(poly));
    CHECK(graph.conductors.contains(met1));
    CHECK(graph.conductors.contains(met2));
    CHECK(!graph.conductors.contains(nwell));
    CHECK(graph.viaConductors.contains(via));
    if (graph.viaConductors.contains(via)) {
        CHECK(graph.viaConductors.at(via).contains(met1));
        CHECK(graph.viaConductors.at(via).contains(met2));
        CHECK(!graph.viaConductors.at(via).contains(diff));
    }
    CHECK(graph.routingLayers.contains(met1));
    CHECK(graph.routingLayers.contains(met2));
    CHECK(!graph.routingLayers.contains(poly));

    if (previous)
        setenv("ICE_PACKAGE_ROOTS", saved.c_str(), 1);
    else
        unsetenv("ICE_PACKAGE_ROOTS");
    std::filesystem::remove_all(root);
}
