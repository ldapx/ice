#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"
#include "icelib/process.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct InterchangeFixture {
    ice::Project project;
    ice::ObjectId cell = 0;
    ice::ObjectId layer = 0;
    ice::ObjectId net = 0;
    ice::ObjectId input = 0;
    ice::ObjectId output = 0;
};

InterchangeFixture makeFixture() {
    InterchangeFixture fixture;
    fixture.project.identity = "interchange";
    fixture.project.name = "interchange";
    fixture.cell = ice::createLayoutCell(fixture.project, "top");
    fixture.layer = ice::addLayer(fixture.project, "Metal1", 10);
    fixture.net = ice::createLayoutNet(fixture.project, fixture.cell, "VIN");
    fixture.input = ice::addPort(fixture.project, fixture.cell, "VIN", fixture.layer, {0, 0, 1000, 500},
                                 ice::PortDirection::input, ice::PortSide::left, fixture.net, "VIN");
    fixture.output = ice::addPort(fixture.project, fixture.cell, "VOUT", fixture.layer, {2000, 0, 500, 1000},
                                  ice::PortDirection::output, ice::PortSide::right, 0, "VOUT");
    return fixture;
}

std::filesystem::path temporaryPath(const std::string &name) {
    return std::filesystem::temp_directory_path() / ("ice-" + name);
}

std::optional<std::filesystem::path> findTool(const std::string &name) {
    const auto check = [](const std::filesystem::path &path) -> std::optional<std::filesystem::path> {
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
        return std::nullopt;
    };
    std::string environmentName = "ICE_";
    for (const char character : name)
        environmentName.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    if (const auto *overridePath = std::getenv(environmentName.c_str()); overridePath && *overridePath) {
        if (const auto found = check(overridePath))
            return found;
    }
    if (const auto *path = std::getenv("PATH"); path && *path) {
        std::stringstream entries(path);
        std::string entry;
        while (std::getline(entries, entry, ':')) {
            if (entry.empty())
                entry = ".";
            if (const auto found = check(std::filesystem::path(entry) / name))
                return found;
        }
    }
    return std::nullopt;
}

bool runKlayoutScript(const std::filesystem::path &script) {
    const auto klayout = findTool("klayout");
    if (!klayout)
        return false;
    ice::ProcessOptions options;
    options.arguments = {klayout->string(), "-b", "-z", "-r", script.string()};
    options.logPath = script.parent_path() / (script.stem().string() + ".log");
    options.timeoutSeconds = 300;
    return ice::runProcess(options).succeeded();
}

std::filesystem::path writeKlayoutScript(const std::string &name, const std::string &body) {
    const auto path = temporaryPath(name + ".py");
    std::ofstream script(path, std::ios::binary | std::ios::trunc);
    script << body;
    return path;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

const ice::Port *portNamed(const ice::Project &project, ice::ObjectId cell, const std::string &name) {
    const auto &ports = project.layoutCells.at(cell).ports;
    const auto found =
        std::find_if(ports.begin(), ports.end(), [&](const auto &entry) { return entry.second.name == name; });
    return found == ports.end() ? nullptr : &found->second;
}
} // namespace

TEST_CASE("LEF export and import round-trip pin metadata") {
    auto fixture = makeFixture();
    const auto path = temporaryPath("pins.lef");
    ice::LayoutInterchange::exportLef(fixture.project, fixture.cell, path);
    const auto text = readFile(path);

    CHECK(text.find("MACRO top") != std::string::npos);
    CHECK(text.find("PIN VIN") != std::string::npos);
    CHECK(text.find("DIRECTION INPUT") != std::string::npos);
    CHECK(text.find("LAYER Metal1") != std::string::npos);
    CHECK(text.find("RECT 0 0 1 0.5") != std::string::npos);

    const auto imported = ice::LayoutInterchange::importLef(path);
    std::filesystem::remove(path);
    CHECK(imported.layoutCells.size() == 1);
    if (imported.layoutCells.size() != 1)
        return;
    const auto &cell = imported.layoutCells.begin()->second;
    CHECK(cell.name == "top");
    CHECK(cell.ports.size() == 2);
    if (cell.ports.size() != 2)
        return;
    const auto *input = portNamed(imported, cell.id, "VIN");
    const auto *output = portNamed(imported, cell.id, "VOUT");
    CHECK(input != nullptr);
    CHECK(output != nullptr);
    if (!input || !output)
        return;
    CHECK(input->direction == ice::PortDirection::input);
    CHECK(output->direction == ice::PortDirection::output);
    CHECK(input->rectangle.x == 0);
    CHECK(input->rectangle.y == 0);
    CHECK(input->rectangle.width == 1000);
    CHECK(input->rectangle.height == 500);
    CHECK(input->layer != 0);
    CHECK(imported.layers.at(input->layer).name == "Metal1");
}

TEST_CASE("LEF exports purpose-layer pins on the base layer") {
    ice::Project project;
    project.identity = "lef-purpose";
    project.name = "lef-purpose";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto layer = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    ice::addPort(project, cell, "A", layer, {0, 0, 1000, 1000}, ice::PortDirection::input, ice::PortSide::left);

    const auto path = temporaryPath("purpose.lef");
    ice::LayoutInterchange::exportLef(project, cell, path);
    const auto text = readFile(path);
    std::filesystem::remove(path);
    CHECK(text.find("LAYER Metal1 ;") != std::string::npos);
    CHECK(text.find("Metal1.pin") == std::string::npos);
}

TEST_CASE("DEF export and import round-trip pins and nets") {
    auto fixture = makeFixture();
    const auto path = temporaryPath("pins.def");
    ice::LayoutInterchange::exportDef(fixture.project, fixture.cell, path);
    const auto text = readFile(path);

    CHECK(text.find("DESIGN top") != std::string::npos);
    CHECK(text.find("PINS 2") != std::string::npos);
    CHECK(text.find("- VIN + NET VIN") != std::string::npos);
    CHECK(text.find("+ DIRECTION OUTPUT") != std::string::npos);

    const auto imported = ice::LayoutInterchange::importDef(path);
    std::filesystem::remove(path);
    CHECK(imported.layoutCells.size() == 1);
    if (imported.layoutCells.size() != 1)
        return;
    const auto &cell = imported.layoutCells.begin()->second;
    CHECK(cell.name == "top");
    CHECK(cell.ports.size() == 2);
    if (cell.ports.size() != 2)
        return;
    const auto *input = portNamed(imported, cell.id, "VIN");
    const auto *output = portNamed(imported, cell.id, "VOUT");
    CHECK(input != nullptr);
    CHECK(output != nullptr);
    if (!input || !output)
        return;
    CHECK(input->direction == ice::PortDirection::input);
    CHECK(output->direction == ice::PortDirection::output);
    CHECK(input->net != 0);
    CHECK(cell.nets.at(input->net).name == "VIN");
    CHECK(input->rectangle.width == 1000);
    CHECK(input->rectangle.height == 500);
    CHECK(output->rectangle.x == 2000);
    CHECK(output->rectangle.height == 1000);
}

TEST_CASE("GDS export and import preserve layer datatypes") {
    ice::Project project;
    project.identity = "gds-datatype";
    project.name = "gds-datatype";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto layer = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    ice::addRectangle(project, cell, layer, {0, 0, 1000, 1000});

    const auto path = temporaryPath("datatype.gds");
    ice::LayoutInterchange::exportGds(project, cell, path);
    const auto imported = ice::LayoutInterchange::importGds(path);
    std::filesystem::remove(path);

    CHECK(imported.layoutCells.size() == 1);
    CHECK(imported.layers.size() == 1);
    if (imported.layers.size() != 1)
        return;
    const auto &restored = imported.layers.begin()->second;
    CHECK(restored.number == 8);
    CHECK(restored.datatype == 2);
    CHECK(restored.name == "GDS layer 8 (dt 2)");
    CHECK(imported.layoutCells.begin()->second.shapes.size() == 1);
}

TEST_CASE("GDS import recovers pin labels when a label layer is configured") {
    auto fixture = makeFixture();
    const auto path = temporaryPath("pins.gds");
    ice::LayoutInterchange::exportGds(fixture.project, fixture.cell, path);

    const auto withoutPins = ice::LayoutInterchange::importGds(path);
    CHECK(withoutPins.layoutCells.size() == 1);
    if (withoutPins.layoutCells.size() != 1)
        return;
    CHECK(withoutPins.layoutCells.begin()->second.ports.empty());

    ice::LayoutImportOptions options;
    options.portLabelLayer = 10;
    options.portWidth = 2000;
    options.portHeight = 3000;
    const auto withPins = ice::LayoutInterchange::importGds(path, options);
    std::filesystem::remove(path);
    CHECK(withPins.layoutCells.size() == 1);
    if (withPins.layoutCells.size() != 1)
        return;
    const auto &cell = withPins.layoutCells.begin()->second;
    CHECK(cell.ports.size() == 2);
    if (cell.ports.size() != 2)
        return;
    const auto *input = portNamed(withPins, cell.id, "VIN");
    const auto *output = portNamed(withPins, cell.id, "VOUT");
    CHECK(input != nullptr);
    CHECK(output != nullptr);
    if (!input || !output)
        return;
    CHECK(input->label == "VIN");
    CHECK(output->label == "VOUT");
    CHECK(input->rectangle.width == 2000);
    CHECK(input->rectangle.height == 3000);
}

TEST_CASE("GDS import preserves paths, boxes, SREF, and AREF hierarchy") {
    if (!findTool("klayout"))
        return;
    const auto gds = temporaryPath("external-hierarchy.gds");
    std::ostringstream body;
    body << "import pya\n"
         << "layout = pya.Layout()\n"
         << "layout.dbu = 0.001\n"
         << "top = layout.create_cell(\"top\")\n"
         << "leaf = layout.create_cell(\"leaf\")\n"
         << "leaf.shapes(layout.layer(1, 0)).insert(pya.Box(0, 0, 100, 100))\n"
         << "top.shapes(layout.layer(2, 0)).insert(pya.Path([pya.Point(0, 0), pya.Point(1000, 0)], 100))\n"
         << "top.shapes(layout.layer(3, 0)).insert(pya.Box(2000, 0, 2100, 100))\n"
         << "top.insert(pya.CellInstArray(leaf.cell_index(), pya.Trans(pya.Vector(4000, 0))))\n"
         << "top.insert(pya.CellInstArray(leaf.cell_index(), pya.Trans(pya.Vector(5000, 0)), "
            "pya.Vector(200, 0), pya.Vector(0, 200), 2, 2))\n"
         << "layout.write(\"" << gds.generic_string() << "\")\n";
    const auto script = writeKlayoutScript("external-hierarchy", body.str());
    REQUIRE(runKlayoutScript(script));
    const auto imported = ice::LayoutInterchange::importGds(gds);
    std::filesystem::remove(gds);
    std::filesystem::remove(script);
    std::filesystem::remove(script.parent_path() / (script.stem().string() + ".log"));

    REQUIRE(imported.layoutCells.size() == 2);
    const auto &top = imported.layoutCells.begin()->second;
    CHECK(top.name == "top");
    CHECK(top.instances.size() == 5);
    CHECK(top.shapes.size() == 2);
    bool foundPath = false;
    bool foundBox = false;
    for (const auto &[_, shape] : top.shapes) {
        if (shape.isPath()) {
            foundPath = shape.centerline.size() == 2 && shape.width == 100;
        } else if (shape.rectangle.x == 2000 && shape.rectangle.width == 100 && shape.rectangle.height == 100) {
            foundBox = true;
        }
    }
    CHECK(foundPath);
    CHECK(foundBox);
    std::multiset<std::pair<ice::Coordinate, ice::Coordinate>> positions;
    for (const auto &[_, instance] : top.instances)
        positions.emplace(instance.x, instance.y);
    CHECK(positions.contains({4000, 0}));
    CHECK(positions.contains({5000, 0}));
    CHECK(positions.contains({5200, 0}));
    CHECK(positions.contains({5000, 200}));
    CHECK(positions.contains({5200, 200}));
}

TEST_CASE("GDS import scales database units to nanometres") {
    if (!findTool("klayout"))
        return;
    const auto gds = temporaryPath("units.gds");
    std::ostringstream body;
    body << "import pya\n"
         << "layout = pya.Layout()\n"
         << "layout.dbu = 0.0005\n"
         << "top = layout.create_cell(\"top\")\n"
         << "top.shapes(layout.layer(1, 0)).insert(pya.Box(0, 0, 1000, 1000))\n"
         << "layout.write(\"" << gds.generic_string() << "\")\n";
    const auto script = writeKlayoutScript("units", body.str());
    REQUIRE(runKlayoutScript(script));
    const auto imported = ice::LayoutInterchange::importGds(gds);
    std::filesystem::remove(gds);
    std::filesystem::remove(script);
    std::filesystem::remove(script.parent_path() / (script.stem().string() + ".log"));
    REQUIRE(imported.layoutCells.size() == 1);
    const auto &cell = imported.layoutCells.begin()->second;
    REQUIRE(cell.shapes.size() == 1);
    CHECK(cell.shapes.begin()->second.rectangle.width == 500);
    CHECK(cell.shapes.begin()->second.rectangle.height == 500);
}

TEST_CASE("OASIS import and export preserve hierarchy and pins") {
    ice::Project project;
    project.identity = "oasis-hierarchy";
    project.name = "oasis-hierarchy";
    const auto layer = ice::addLayer(project, "Metal1", 34);
    const auto leaf = ice::createLayoutCell(project, "leaf");
    ice::addRectangle(project, leaf, layer, ice::Rect{0, 0, 100, 100});
    ice::addPort(project, leaf, "IN", layer, {0, 0, 10, 10}, ice::PortDirection::input, ice::PortSide::left, 0, "IN");
    const auto parent = ice::createLayoutCell(project, "parent");
    ice::addLayoutInstance(project, parent, leaf, "U1", 1000, 2000, ice::Orientation::r90);

    const auto oasis = temporaryPath("hierarchy.oas");
    ice::LayoutInterchange::exportStream(project, parent, oasis, false);
    ice::LayoutImportOptions options;
    options.portLabelLayer = 34;
    const auto imported = ice::LayoutInterchange::importOasis(oasis, options);
    CHECK(imported.layoutCells.size() == 2);
    const auto &top = imported.layoutCells.begin()->second;
    CHECK(top.name == "parent");
    REQUIRE(top.instances.size() == 1);
    CHECK(top.instances.begin()->second.orientation == ice::Orientation::r90);
    const auto child = imported.layoutCells.find(top.instances.begin()->second.cell);
    REQUIRE(child != imported.layoutCells.end());
    CHECK(child->second.name == "leaf");
    CHECK(child->second.ports.size() == 1);

    const auto inMemoryFlat = ice::LayoutInterchange::flattenProject(imported, top.id);
    REQUIRE(inMemoryFlat.layoutCells.size() == 1);
    CHECK(inMemoryFlat.layoutCells.begin()->second.instances.empty());
    CHECK(!inMemoryFlat.layoutCells.begin()->second.shapes.empty());

    const auto flat = temporaryPath("hierarchy-flat.oas");
    ice::LayoutInterchange::exportStream(project, parent, flat, true);
    const auto flattened = ice::LayoutInterchange::importStream(flat, options);
    CHECK(flattened.layoutCells.size() == 1);
    CHECK(flattened.layoutCells.begin()->second.instances.empty());
    bool foundLeaf = false;
    for (const auto &[_, shape] : flattened.layoutCells.begin()->second.shapes) {
        if (shape.rectangle.width == 100 && shape.rectangle.height == 100)
            foundLeaf = true;
    }
    CHECK(foundLeaf);
    std::filesystem::remove(oasis);
    std::filesystem::remove(flat);
}

TEST_CASE("OASIS instance orientations round-trip") {
    const std::array<ice::Orientation, 8> orientations = {
        ice::Orientation::r0, ice::Orientation::r90, ice::Orientation::r180, ice::Orientation::r270,
        ice::Orientation::mx, ice::Orientation::my,  ice::Orientation::m45,  ice::Orientation::m135,
    };
    for (const auto orientation : orientations) {
        ice::Project project;
        project.identity = "oasis-orientation";
        project.name = "oasis-orientation";
        const auto child = ice::createLayoutCell(project, "child");
        const auto parent = ice::createLayoutCell(project, "parent");
        ice::addLayoutInstance(project, parent, child, "U1", 100, 200, orientation);
        const auto oasis = temporaryPath("orientation.oas");
        ice::LayoutInterchange::exportStream(project, parent, oasis, false);
        const auto imported = ice::LayoutInterchange::importStream(oasis);
        std::filesystem::remove(oasis);
        REQUIRE(imported.layoutCells.size() == 2);
        const auto &top = imported.layoutCells.begin()->second;
        REQUIRE(top.instances.size() == 1);
        CHECK(top.instances.begin()->second.orientation == orientation);
        CHECK(top.instances.begin()->second.x == 100);
        CHECK(top.instances.begin()->second.y == 200);
    }
}

TEST_CASE("GDS export and import preserve simple path records") {
    ice::Project project;
    project.identity = "gds-path";
    project.name = "gds-path";
    const auto layer = ice::addLayer(project, "Metal1", 34);
    const auto cell = ice::createLayoutCell(project, "top");
    const auto pathId = project.nextId();
    ice::Shape path{pathId, layer, std::nullopt, {0, 0, 1000, 500}, {}};
    path.centerline = {{0, 0}, {1000, 0}, {1000, 500}};
    path.width = 100;
    project.layoutCells.at(cell).shapes.emplace(pathId, path);

    const auto gds = temporaryPath("path.gds");
    ice::LayoutInterchange::exportGds(project, cell, gds);
    const auto imported = ice::LayoutInterchange::importGds(gds);
    std::filesystem::remove(gds);
    REQUIRE(imported.layoutCells.size() == 1);
    const auto &importedCell = imported.layoutCells.begin()->second;
    REQUIRE(importedCell.shapes.size() == 1);
    const auto &importedPath = importedCell.shapes.begin()->second;
    CHECK(importedPath.isPath());
    CHECK(importedPath.width == 100);
    REQUIRE(importedPath.centerline.size() == 3);
    CHECK(importedPath.centerline[2].x == 1000);
    CHECK(importedPath.centerline[2].y == 500);
}

TEST_CASE("GDS instance orientations round-trip") {
    const std::array<ice::Orientation, 8> orientations = {
        ice::Orientation::r0, ice::Orientation::r90, ice::Orientation::r180, ice::Orientation::r270,
        ice::Orientation::mx, ice::Orientation::my,  ice::Orientation::m45,  ice::Orientation::m135,
    };
    for (const auto orientation : orientations) {
        ice::Project project;
        project.identity = "gds-orientation";
        project.name = "gds-orientation";
        const auto child = ice::createLayoutCell(project, "child");
        const auto parent = ice::createLayoutCell(project, "parent");
        ice::addLayoutInstance(project, parent, child, "U1", 100, 200, orientation);
        const auto gds = temporaryPath("orientation.gds");
        ice::LayoutInterchange::exportGds(project, parent, gds);
        const auto imported = ice::LayoutInterchange::importGds(gds);
        std::filesystem::remove(gds);
        REQUIRE(imported.layoutCells.size() == 2);
        const auto &top = imported.layoutCells.begin()->second;
        REQUIRE(top.instances.size() == 1);
        CHECK(top.instances.begin()->second.orientation == orientation);
        CHECK(top.instances.begin()->second.x == 100);
        CHECK(top.instances.begin()->second.y == 200);
    }
}
