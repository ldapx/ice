#include "icelib/connectivity.hpp"
#include "icelib/operations.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
