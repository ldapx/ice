#include "icelib/drc.hpp"
#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

    const auto report = ice::parseGdsCheckLyrdbText(text);
    CHECK(report.drc.size() == 2);
    const auto &density = report.drc[0];
    CHECK(density.severity == ice::VerificationSeverity::error);
    CHECK(density.rule == "M2.Fil");
    CHECK(density.cell == "TOP");
    CHECK(density.message.find("Minimum windowed density violation") != std::string::npos);
    CHECK(density.message.find("windowed density 0.00% < 25.00%") != std::string::npos);
    CHECK(density.location.x == -240);
    CHECK(density.location.y == -220);
    CHECK(density.location.width == 4320);
    CHECK(density.location.height == 4390);

    const auto &spacing = report.drc[1];
    CHECK(spacing.severity == ice::VerificationSeverity::warning);
    CHECK(spacing.rule == "M3.k");
    CHECK(spacing.message.find("Recommended spacing") != std::string::npos);
    CHECK(spacing.location.width == 0);
    CHECK(spacing.location.height == 0);
}

TEST_CASE("gdscheck lyrdb without items is a clean report") {
    const auto report = ice::parseGdsCheckLyrdbText(
        "<?xml version=\"1.0\"?><report-database><categories></categories><items></items></report-database>");
    CHECK(report.drc.empty());
    CHECK(report.lvs.empty());
    CHECK(report.pex.empty());
}

TEST_CASE("layout GDS export writes gdscheck-compatible records") {
    ice::Project project;
    project.identity = "drc-gds";
    project.name = "drc-gds";
    const auto cell = ice::createLayoutCell(project, "top", "ihp.sg13g2");
    const auto layer = ice::addLayer(project, "Activ", 1);
    ice::addRectangle(project, cell, layer, {0, 0, 1000, 1000});
    const auto path = std::filesystem::temp_directory_path() / "ice-drc-export.gds";
    ice::LayoutInterchange::exportGds(project, cell, path);
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
    ice::Project project;
    project.identity = "flatten";
    project.name = "flatten";
    const auto top = ice::createLayoutCell(project, "top", "ihp.sg13g2");
    const auto device = ice::createLayoutCell(project, "device", "ihp.sg13g2");
    project.layoutCells.at(device).library = true;
    project.layoutCells.at(device).device = "nmos";
    const auto layer = ice::addLayer(project, "Activ", 1);
    ice::addRectangle(project, device, layer, {0, 0, 10, 20});
    const auto instance = project.nextId();
    project.layoutCells.at(top).instances.emplace(
        instance, ice::LayoutInstance{instance, device, "device_1", 100, 50, ice::Orientation::r90});

    const auto path = std::filesystem::temp_directory_path() / "ice-drc-flattened.gds";
    ice::LayoutInterchange::exportFlattenedGds(project, top, path);
    const auto imported = ice::LayoutInterchange::importGds(path);
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
    CHECK_THROWS_AS(ice::parseGdsCheckLyrdbText("<not-a-report/>"), ice::ProjectError);
}

TEST_CASE("gdscheck process and argument mapping is stable") {
    CHECK(ice::gdsCheckProcessForPdk("ihp.sg13g2") == "ihp-sg13g2");
    CHECK(ice::gdsCheckProcessForPdk("IHP SG13G2") == "ihp-sg13g2");
    CHECK(ice::gdsCheckProcessForPdk("ihp.sg13cmos5l") == "ihp-sg13cmos5l");
    CHECK(ice::gdsCheckProcessForPdk("gf180mcuA") == "gf180mcuA");
    CHECK(ice::gdsCheckProcessForPdk("gf180mcuD") == "gf180mcuD");
    CHECK(ice::gdsCheckProcessForPdk("icsprout55") == "icsprout55");
    CHECK(ice::gdsCheckProcessForPdk(ice::genericPdkId).empty());

    const ice::GdsCheckOptions options{"/tmp/layout.gds", "/tmp/drc.lyrdb", "ihp-sg13g2", "TOP", "main"};
    CHECK((ice::gdsCheckArguments(options) == std::vector<std::string>{"run", "--input", "/tmp/layout.gds", "--process",
                                                                       "ihp-sg13g2", "--topcell", "TOP", "--report",
                                                                       "/tmp/drc.lyrdb", "--suite", "main"}));
}
