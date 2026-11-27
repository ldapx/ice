#include "openvlsilib/drc.hpp"
#include "openvlsilib/interchange.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/pdk.hpp"
#include "openvlsilib/verification.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("package DRC deck configuration resolves KLayout decks and suites") {
    const auto root = std::filesystem::temp_directory_path() / "openvlsi-drc-package-test";
    std::filesystem::remove_all(root);
    const auto package = root / "testdrc";
    std::filesystem::create_directories(package / "technology");
    std::ofstream(package / "package.toml") << R"([package]
schema_version = 1
name = "test.drc"
version = "1.0.0"
license = "Apache-2.0"

[provides]
pdk = true

[pdk]
id = "testdrc"
display_name = "Test DRC"
)";
    std::ofstream(package / "technology" / "test.lydrc") << "report(\"test\", $report)\n";
    std::ofstream(package / "technology" / "drc.json") << R"({
  "format": "openvlsi-drc",
  "schema_version": 1,
  "engine": "klayout",
  "deck": "test.lydrc",
  "arguments": ["-b", "-r", "{deck}"],
  "suites": {
    "beol": {
      "deck": "test.lydrc",
      "arguments": ["-b", "-r", "{deck}", "-rd", "beol=1"]
    }
  }
})";
    openvlsi::setPdkPackageRoots({root});
    const auto mainDeck = openvlsi::drcDeckForPdk("testdrc", "main");
    CHECK(mainDeck.has_value());
    if (mainDeck) {
        CHECK(mainDeck->engine == "klayout");
        CHECK(mainDeck->path.filename() == "test.lydrc");
        CHECK(mainDeck->arguments.size() == 3);
    }
    const auto beolDeck = openvlsi::drcDeckForPdk("testdrc", "beol");
    CHECK(beolDeck.has_value());
    if (beolDeck) {
        CHECK(beolDeck->path.filename() == "test.lydrc");
        CHECK(beolDeck->arguments.size() == 5);
    }
    openvlsi::setPdkPackageRoots({});
    std::filesystem::remove_all(root);
}

TEST_CASE("gdscheck lyrdb reports become normalized DRC violations") {
    const std::string text = R"XML(<?xml version="1.0" encoding="utf-8"?>
<report-database>
  <description>gdscheck DRC results</description>
  <categories>
    <category>
      <name>M2</name>
      <categories>
        <category>
          <name>Fil</name>
          <description>Minimum windowed density violation</description>
        </category>
      </categories>
    </category>
    <category>
      <name>M3</name>
      <categories>
        <category>
          <name>k</name>
          <description>Recommended spacing</description>
        </category>
      </categories>
    </category>
  </categories>
  <items>
    <item>
      <category>M2.Fil</category>
      <cell>TOP</cell>
      <visited>false</visited>
      <multiplicity>1</multiplicity>
      <values>
        <value>edge:(-0.2400,-0.2200;4.0800,4.1700)</value>
        <value>text: &apos;windowed density 0.00% &lt; 25.00%&apos;</value>
      </values>
    </item>
    <item>
      <category>M3.k</category>
      <cell>TOP</cell>
      <severity>warning</severity>
      <values>
        <value>text: &apos;spacing 0.15 &lt; 0.20&apos;</value>
      </values>
    </item>
  </items>
</report-database>
)XML";

    const auto report = openvlsi::parseGdsCheckLyrdbText(text);
    CHECK(report.drc.size() == 2);
    const auto &density = report.drc[0];
    CHECK(density.severity == openvlsi::VerificationSeverity::error);
    CHECK(density.rule == "M2.Fil");
    CHECK(density.cell == "TOP");
    CHECK(density.message.find("Minimum windowed density violation") != std::string::npos);
    CHECK(density.message.find("windowed density 0.00% < 25.00%") != std::string::npos);
    CHECK(density.location.x == -240);
    CHECK(density.location.y == -220);
    CHECK(density.location.width == 4320);
    CHECK(density.location.height == 4390);

    const auto &spacing = report.drc[1];
    CHECK(spacing.severity == openvlsi::VerificationSeverity::warning);
    CHECK(spacing.rule == "M3.k");
    CHECK(spacing.message.find("Recommended spacing") != std::string::npos);
    CHECK(spacing.location.width == 0);
    CHECK(spacing.location.height == 0);
}

TEST_CASE("gdscheck lyrdb without items is a clean report") {
    const auto report = openvlsi::parseGdsCheckLyrdbText(
        "<?xml version=\"1.0\"?><report-database><categories></categories><items></items></report-database>");
    CHECK(report.drc.empty());
    CHECK(report.lvs.empty());
    CHECK(report.pex.empty());
}

TEST_CASE("layout GDS export writes gdscheck-compatible records") {
    openvlsi::Project project;
    project.identity = "drc-gds";
    project.name = "drc-gds";
    const auto cell = openvlsi::createLayoutCell(project, "top", "ihp.sg13g2");
    const auto layer = openvlsi::addLayer(project, "Activ", 1);
    openvlsi::addRectangle(project, cell, layer, {0, 0, 1000, 1000});
    const auto path = std::filesystem::temp_directory_path() / "openvlsi-drc-export.gds";
    openvlsi::LayoutInterchange::exportGds(project, cell, path);
    std::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::filesystem::remove(path);
    CHECK(bytes.size() > 100);

    struct Record {
        std::uint8_t type = 0;
        std::uint8_t dataType = 0;
        std::vector<unsigned char> data;
    };
    std::vector<Record> records;
    std::size_t position = 0;
    while (position + 4 <= bytes.size()) {
        const auto length = static_cast<std::size_t>((static_cast<unsigned int>(bytes[position]) << 8) |
                                                     static_cast<unsigned int>(bytes[position + 1]));
        if (length < 4 || position + length > bytes.size())
            break;
        records.push_back({bytes[position + 2], bytes[position + 3],
                           std::vector<unsigned char>(bytes.begin() + static_cast<std::ptrdiff_t>(position + 4),
                                                      bytes.begin() + static_cast<std::ptrdiff_t>(position + length))});
        position += length;
    }
    CHECK(!records.empty());
    if (!records.empty()) {
        CHECK(records.front().type == 0);
        CHECK((records.front().data == std::vector<unsigned char>{0, 5}));
    }
    bool sawBeginLibrary = false;
    bool sawBeginStructure = false;
    bool sawBoundary = false;
    bool sawLayerAfterBoundary = false;
    bool sawDataTypeAfterLayer = false;
    for (const auto &record : records) {
        if (record.type == 1)
            sawBeginLibrary = record.data.size() == 24;
        else if (record.type == 5)
            sawBeginStructure = record.data.size() == 24;
        else if (record.type == 8)
            sawBoundary = true;
        else if (record.type == 13 && sawBoundary)
            sawLayerAfterBoundary = true;
        else if (record.type == 14 && sawLayerAfterBoundary)
            sawDataTypeAfterLayer = true;
    }
    CHECK(sawBeginLibrary);
    CHECK(sawBeginStructure);
    CHECK(sawBoundary);
    CHECK(sawLayerAfterBoundary);
    CHECK(sawDataTypeAfterLayer);
    if (!records.empty())
        CHECK(records.back().type == 4);
}

TEST_CASE("flattened GDS export includes transformed instance geometry") {
    openvlsi::Project project;
    project.identity = "flatten";
    project.name = "flatten";
    const auto top = openvlsi::createLayoutCell(project, "top", "ihp.sg13g2");
    const auto device = openvlsi::createLayoutCell(project, "device", "ihp.sg13g2");
    project.layoutCells.at(device).library = true;
    project.layoutCells.at(device).device = "nmos";
    const auto layer = openvlsi::addLayer(project, "Activ", 1);
    openvlsi::addRectangle(project, device, layer, {0, 0, 10, 20});
    const auto instance = project.nextId();
    project.layoutCells.at(top).instances.emplace(
        instance, openvlsi::LayoutInstance{instance, device, "device_1", 100, 50, openvlsi::Orientation::r90});

    const auto path = std::filesystem::temp_directory_path() / "openvlsi-drc-flattened.gds";
    openvlsi::LayoutInterchange::exportFlattenedGds(project, top, path);
    const auto imported = openvlsi::LayoutInterchange::importGds(path);
    std::filesystem::remove(path);

    CHECK(imported.layoutCells.size() == 1);
    if (imported.layoutCells.empty())
        return;
    const auto &cell = imported.layoutCells.begin()->second;
    CHECK(cell.shapes.size() == 1);
    if (cell.shapes.empty())
        return;
    const auto &shape = cell.shapes.begin()->second;
    CHECK(shape.rectangle.x == 80);
    CHECK(shape.rectangle.y == 50);
    CHECK(shape.rectangle.width == 20);
    CHECK(shape.rectangle.height == 10);
}

TEST_CASE("invalid lyrdb input is rejected") {
    CHECK_THROWS_AS(openvlsi::parseGdsCheckLyrdbText("<not-a-report/>"), openvlsi::ProjectError);
}

TEST_CASE("truncated lyrdb reports are rejected") {
    CHECK_THROWS_AS(openvlsi::parseGdsCheckLyrdbText("<report-database><items></items>"), openvlsi::ProjectError);
    CHECK_THROWS_AS(
        openvlsi::parseGdsCheckLyrdbText("<report-database><categories></categories><items><item><category>A</category>"),
        openvlsi::ProjectError);
    CHECK(openvlsi::parseGdsCheckLyrdbText("<report-database><categories></categories><items></items></report-database>")
              .drc.empty());
}

TEST_CASE("layout DRC rejects a nonzero tool exit with a clean report") {
    const auto root = std::filesystem::temp_directory_path() / "openvlsi-drc-status-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto script = root / "gdscheck";
    std::ofstream(script) << "#!/bin/sh\n"
                             "report=\"\"\n"
                             "while [ $# -gt 0 ]; do\n"
                             "  if [ \"$1\" = \"--report\" ]; then shift; report=\"$1\"; fi\n"
                             "  shift\n"
                             "done\n"
                             "if [ -n \"$report\" ]; then\n"
                             "  printf '<report-database><categories></categories><items></items></report-database>' > "
                             "\"$report\"\n"
                             "fi\n"
                             "exit 3\n";
    std::filesystem::permissions(script,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    const auto previousPath = std::getenv("PATH") ? std::string(std::getenv("PATH")) : std::string{};
    setenv("PATH", (root.string() + ":" + previousPath).c_str(), 1);

    openvlsi::Project project;
    project.identity = "drc-status";
    project.name = "drc-status";
    const auto cell = openvlsi::createLayoutCell(project, "top", "ihp.sg13g2");
    const auto layer = openvlsi::addLayer(project, "Activ", 1);
    openvlsi::addRectangle(project, cell, layer, {0, 0, 1000, 1000});
    openvlsi::LayoutDrcOptions options;
    options.workDirectory = root / "work";
    options.engine = "gdscheck";
    const auto result = openvlsi::runLayoutDrc(project, cell, options);

    setenv("PATH", previousPath.c_str(), 1);
    std::filesystem::remove_all(root);
    CHECK(!result.ok);
    CHECK(result.error.find("no DRC violations") != std::string::npos);
}

TEST_CASE("package DRC configuration declares the gdscheck process") {
    const auto ihp = openvlsi::drcDeckForPdk("ihp-sg13g2");
    if (ihp) {
        CHECK(ihp->engine == "gdscheck");
        CHECK(ihp->process == "ihp-sg13g2");
    }
    const auto gf = openvlsi::drcDeckForPdk("gf180mcuA");
    if (gf) {
        CHECK(gf->engine == "gdscheck");
        CHECK(gf->process == "gf180mcuA");
    }
    const auto sky = openvlsi::drcDeckForPdk("sky130A");
    if (sky) {
        CHECK(sky->engine == "klayout");
        CHECK(sky->process == "sky130A");
    }
}

TEST_CASE("gdscheck process and argument mapping is stable") {
    CHECK(openvlsi::gdsCheckProcessForPdk("ihp.sg13g2") == "ihp-sg13g2");
    CHECK(openvlsi::gdsCheckProcessForPdk("IHP SG13G2") == "ihp-sg13g2");
    CHECK(openvlsi::gdsCheckProcessForPdk("ihp.sg13cmos5l") == "ihp-sg13cmos5l");
    CHECK(openvlsi::gdsCheckProcessForPdk("gf180mcuA") == "gf180mcuA");
    CHECK(openvlsi::gdsCheckProcessForPdk("gf180mcuD") == "gf180mcuD");
    CHECK(openvlsi::gdsCheckProcessForPdk("icsprout55") == "icsprout55");
    CHECK(openvlsi::gdsCheckProcessForPdk(openvlsi::genericPdkId).empty());

    const openvlsi::GdsCheckOptions options{"/tmp/layout.gds", "/tmp/drc.lyrdb", "ihp-sg13g2", "TOP", "main"};
    CHECK((openvlsi::gdsCheckArguments(options) == std::vector<std::string>{"run", "--input", "/tmp/layout.gds", "--process",
                                                                       "ihp-sg13g2", "--topcell", "TOP", "--report",
                                                                       "/tmp/drc.lyrdb", "--suite", "main"}));
}

TEST_CASE("local verification checks port geometry and port shorts") {
    openvlsi::Project project;
    project.identity = "pin-drc";
    project.name = "pin-drc";
    project.grid = 10;
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto layer = openvlsi::addLayer(project, "Metal1", 8);
    const auto power = openvlsi::createLayoutNet(project, cell, "VDD");
    const auto ground = openvlsi::createLayoutNet(project, cell, "VSS");
    openvlsi::addRectangle(project, cell, layer, {0, 0, 4000, 4000});
    openvlsi::addPort(project, cell, "VDD", layer, {0, 0, 1000, 1000}, openvlsi::PortDirection::power, openvlsi::PortSide::left, power,
                 "VDD");
    openvlsi::addPort(project, cell, "VSS", layer, {500, 500, 1000, 1000}, openvlsi::PortDirection::ground, openvlsi::PortSide::bottom,
                 ground, "VSS");
    openvlsi::addPort(project, cell, "NARROW", layer, {2000, 2000, 1, 1000}, openvlsi::PortDirection::input, openvlsi::PortSide::right,
                 0, "NARROW");
    openvlsi::addPort(project, cell, "OUTSIDE", layer, {5000, 5000, 500, 500}, openvlsi::PortDirection::output,
                 openvlsi::PortSide::right, 0, "OUTSIDE");

    openvlsi::LocalVerificationAdapter adapter;
    const auto report = adapter.run(project, cell);
    const auto hasRule = [&](const std::string &rule) {
        return std::any_of(report.drc.begin(), report.drc.end(),
                           [&](const openvlsi::DrcViolation &violation) { return violation.rule == rule; });
    };
    CHECK(hasRule("port-short"));
    CHECK(hasRule("port-min-width"));
    CHECK(hasRule("port-outside-cell"));
}
