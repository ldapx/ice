#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

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
    CHECK(imported.layers.at(input->layer).name == "Metal1");
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
    options.pinLabelLayer = 10;
    options.pinWidth = 2000;
    options.pinHeight = 3000;
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
