#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ice {

using ObjectId = std::uint64_t;
using Coordinate = std::int64_t;

inline constexpr const char *genericPdkId = "generic";

struct Diagnostic {
    enum class Severity { error, warning };
    Severity severity;
    std::string code;
    std::string message;
    std::optional<ObjectId> object;
};

class ProjectError : public std::runtime_error {
  public:
    ProjectError(std::string code, std::string message);
    const std::string &code() const noexcept;

  private:
    std::string code_;
};

enum class Orientation { r0, r90, r180, r270, mx, my, m45, m135 };

struct DatabaseUnits {
    std::int64_t nanometers_per_unit = 1;
    std::int64_t denominator = 1;
    double toNanometers(Coordinate value) const;
    bool valid() const;
};

enum class PinDirection { left, right, up, down };

struct Pin {
    ObjectId id = 0;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    PinDirection direction = PinDirection::right;
};

struct Parameter {
    ObjectId id = 0;
    std::string name;
    std::string value;
};

struct SymbolLabel {
    Coordinate x = 0;
    Coordinate y = 0;
};

struct Symbol {
    ObjectId id = 0;
    std::string name;
    std::string artwork;
    // Source schematic for hierarchical block symbols. Zero for primitive or
    // library symbols that are not backed by a schematic cell.
    ObjectId sourceCell = 0;
    // Optional label anchor in symbol coordinates (origin at the artwork
    // center, positive Y upward). The instance name is centered on this point.
    std::optional<SymbolLabel> label;
    std::vector<Pin> pins;
};

struct SchematicInstance {
    ObjectId id = 0;
    ObjectId symbol = 0;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
    std::vector<Parameter> parameters;
};

struct Net {
    ObjectId id = 0;
    std::string name;
    std::vector<ObjectId> pins;
    Coordinate x = 0;
    Coordinate y = 0;
};

struct SchematicComment {
    ObjectId id = 0;
    std::string text;
    Coordinate x = 0;
    Coordinate y = 0;
};

struct SchematicCell {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    // Default parameters of the symbol that represents this schematic when it
    // is used as a hierarchical block in another schematic.
    std::vector<Parameter> parameters;
    std::map<ObjectId, Symbol> symbols;
    std::map<ObjectId, SchematicInstance> instances;
    std::map<ObjectId, Net> nets;
    std::map<ObjectId, SchematicComment> comments;
    // True for a schematic authored as a raw SPICE text document rather than
    // as a drawn symbol-and-net canvas.
    bool textSchematic = false;
    std::string spiceText;
};

struct Layer {
    ObjectId id = 0;
    std::string name;
    int number = 0;
    bool visible = true;
    // Zero means this is a legacy/project-wide layer. Non-zero custom layers
    // belong only to that layout cell; pdk layers are shared by matching PDKs.
    ObjectId layout = 0;
    std::string pdk;
    // GDSII datatype/purpose. Layers with the same number but different
    // datatypes are separate entries so geometry and pins keep their exact
    // stream identity. The Layers panel groups them by number for display.
    int datatype = 0;
    // Empty color and pattern -1 mean the UI derives both from the layer
    // number. Explicit values let users style custom layers.
    std::string color;
    int pattern = -1;
};

struct Rect {
    Coordinate x = 0;
    Coordinate y = 0;
    Coordinate width = 0;
    Coordinate height = 0;
    bool valid() const;
};

struct LayoutPoint {
    Coordinate x = 0;
    Coordinate y = 0;
};

struct Shape {
    ObjectId id = 0;
    ObjectId layer = 0;
    std::optional<ObjectId> net;
    Rect rectangle;
    std::vector<LayoutPoint> polygon;
    // Non-empty for routed paths. The centerline is the editable geometry;
    // polygon is the cached outline used for rendering, DRC, and GDS export.
    std::vector<LayoutPoint> centerline;
    // Layer of each centerline segment (size centerline.size() - 1). Empty
    // means every segment uses `layer`.
    std::vector<ObjectId> segmentLayers;
    // Via layers at each centerline point (size centerline.size()). Each inner
    // vector lists the via layers stacked at that point.
    std::vector<std::vector<ObjectId>> pointVias;
    Coordinate width = 0;
    // Non-empty for layout text. rectangle stores the nominal text box; x/y is
    // the lower-left corner and height is the text size in database units.
    std::string text;
    // Optional hierarchy display name. Shapes without a name use a generated
    // type/id label in the layout hierarchy.
    std::string name;
    // Text-polygon pieces share textGroup. The group id is the first piece's
    // shape id so existing shape selection and references keep working.
    bool textPolygon = false;
    ObjectId textGroup = 0;
    std::string textSource;
    bool isPath() const { return centerline.size() >= 2 && width > 0; }
    bool isText() const { return !text.empty(); }
    ObjectId layerForSegment(std::size_t index) const {
        return index < segmentLayers.size() ? segmentLayers[index] : layer;
    }
};

enum class PortDirection { input, output, inout, power, ground };
enum class PortSide { left, right, top, bottom };

const char *portDirectionName(PortDirection direction);
const char *portSideName(PortSide side);
std::optional<PortDirection> parsePortDirection(const std::string &value);
std::optional<PortSide> parsePortSide(const std::string &value);

struct LayoutNet {
    ObjectId id = 0;
    std::string name;
};

struct Port {
    ObjectId id = 0;
    std::string name;
    ObjectId layer = 0;
    Rect rectangle;
    PortDirection direction = PortDirection::inout;
    PortSide side = PortSide::left;
    // LayoutNet id, or zero when the pin is intentionally unassigned.
    ObjectId net = 0;
    std::string label;
    bool visible = true;
    bool locked = false;
    // Set when this pin is promoted from a child layout instance.
    ObjectId sourceInstance = 0;
    ObjectId sourcePort = 0;
};

struct LayoutInstance {
    ObjectId id = 0;
    ObjectId cell = 0;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
};

// A movable placeholder for a device that has not been implemented as real
// layout geometry yet. Planned devices are projected from a schematic and live
// directly under the layout's root layer. x/y is the lower-left corner in
// database units; the canvas draws the box upward from that point.
struct PlannedDevice {
    ObjectId id = 0;
    std::string name;
    // Schematic instance this placeholder was derived from, when known.
    ObjectId schematicInstance = 0;
    Coordinate x = 0;
    Coordinate y = 0;
    Coordinate width = 0;
    Coordinate height = 0;
};

// A straight logical connection between two planned devices. The net points at
// a LayoutNet in the same cell.
struct PlannedConnection {
    ObjectId id = 0;
    ObjectId from = 0;
    ObjectId to = 0;
    ObjectId net = 0;
};

struct LayoutCell {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    // True for cells imported from a read-only device library.
    bool library = false;
    // Source device name in the bundled layout library. Empty for ordinary
    // user cells and non-device library cells.
    std::string device;
    // PCell metadata for imported library cells. Geometry is stored as
    // flattened polygons and can be regenerated from these values.
    std::vector<Parameter> parameters;
    std::map<ObjectId, Shape> shapes;
    std::map<ObjectId, Port> ports;
    std::map<ObjectId, LayoutNet> nets;
    std::map<ObjectId, LayoutInstance> instances;
    std::map<ObjectId, PlannedDevice> plannedDevices;
    std::map<ObjectId, PlannedConnection> plannedConnections;
    // Non-zero when this cell is the static output of a procedural layout.
    // Generated cells stay ordinary layout cells so they can be inspected,
    // exported, and used as instances by other cells.
    ObjectId generatedBy = 0;
};

// A layout whose geometry is produced by a Python script. The generated
// static layout lives in generatedCell; regenerating the script replaces that
// cell's contents while keeping the procedural layout as the source of truth.
struct ProceduralLayout {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    std::string script;
    ObjectId generatedCell = 0;
};

struct SimulationGraph {
    ObjectId id = 0;
    std::string name;
    // Axis selections are signal names from the simulation's captured signal
    // list. The literal "time" selects the simulator's time vector. X selects
    // one signal; Y can contain multiple signals sharing the same X axis.
    std::string xAxis = "time";
    std::vector<std::string> ySignals;
    // Optional display-only label overrides keyed by signal name. Labels are
    // used for the axis titles and the plot legend; they never change which
    // signal is read from the simulation result.
    std::map<std::string, std::string> signalLabels;
    // Optional display label for the Y axis title. Empty uses the default
    // "Signals" caption.
    std::string yAxisLabel;
    // Optional display limits. Empty values auto-scale that axis from the
    // result data; they are presentation-only and never affect the simulation
    // request, netlist, or data acquisition.
    std::optional<double> xMin;
    std::optional<double> xMax;
    std::optional<double> yMin;
    std::optional<double> yMax;
};

struct SimulationItem {
    ObjectId id = 0;
    std::string name;
    ObjectId schematic = 0;
    std::string simulator = "ngspice";
    std::string analysis = "transient";
    double stop = 1.0;
    double step = 0.01;
    bool use_initial_conditions = false;
    std::string directives;
    // Comma-separated signals requested from the simulator. Axis selections in
    // each graph reference entries in this list, or the special "time" value.
    std::string captureSignals;
    std::vector<SimulationGraph> graphs;
};

// A project-level Python simulation script. Scripts are stored in scripts/ and
// can read any simulation run in the project.
struct ProjectReport {
    ObjectId id = 0;
    std::string name;
    std::string path;
};

struct ProjectLoadOptions {
    // When true, malformed sidecar files are moved under build/recovery/ and
    // the remaining project is loaded. Strict loading remains the default.
    bool recover = false;
};

struct ProjectLoadReport {
    std::vector<std::string> warnings;
    std::vector<std::string> quarantinedFiles;
};

class Project {
  public:
    static constexpr int schemaVersion = 2;

    std::string identity;
    std::string name;
    DatabaseUnits units;
    Coordinate grid = 1;
    std::vector<std::string> packageDependencies;
    std::map<ObjectId, SchematicCell> schematicCells;
    std::map<ObjectId, Layer> layers;
    std::map<ObjectId, LayoutCell> layoutCells;
    std::map<ObjectId, ProceduralLayout> proceduralLayouts;
    std::map<ObjectId, SimulationItem> simulations;
    std::map<ObjectId, ProjectReport> reports;

    ObjectId nextId();
    Coordinate snap(Coordinate value) const;
    std::vector<Diagnostic> validate() const;
    std::string serialize() const;
    static Project deserialize(const std::string &text);
    // Saves/loads an ICE project directory. `path` may be the project root or
    // its project.icepro file. The project file contains only project metadata;
    // schematics, layouts, and simulations are stored in sch/, lay/, and sim/.
    void save(const std::filesystem::path &path) const;
    static Project load(const std::filesystem::path &path);
    static Project load(const std::filesystem::path &path, const ProjectLoadOptions &options,
                        ProjectLoadReport *report = nullptr);

    // Revision of the split files as they existed when this project was loaded
    // or last saved. Empty for a project that has never been loaded or saved.
    const std::string &sourceRevision() const noexcept { return sourceRevision_; }

  private:
    mutable std::filesystem::path sourceRoot_;
    mutable std::string sourceRevision_;
};

class SelectionModel {
  public:
    void select(ObjectId id, std::vector<ObjectId> related = {});
    void clear();
    std::optional<ObjectId> primary() const noexcept;
    const std::vector<ObjectId> &related() const noexcept;

  private:
    std::optional<ObjectId> primary_;
    std::vector<ObjectId> related_;
};

class ProjectSession {
  public:
    using Observer = std::function<void()>;

    explicit ProjectSession(Project project = {});
    Project &project() noexcept;
    const Project &project() const noexcept;
    bool dirty() const noexcept;
    const std::vector<std::string> &history() const noexcept;
    std::size_t historyPosition() const noexcept;
    void setLastHistoryLabel(std::string label);
    bool jumpToHistory(std::size_t position);
    void replaceProject(Project project);
    void markDirty();
    void subscribe(Observer observer);
    void beginTransaction(std::string label);
    void commitTransaction();
    void rollbackTransaction();
    void execute(std::string label, const std::function<void(Project &)> &mutation);
    bool undo();
    bool redo();
    void save(const std::filesystem::path &path);

  private:
    Project project_;
    std::vector<Project> undo_;
    std::vector<Project> redo_;
    std::optional<Project> transactionSnapshot_;
    std::string transactionLabel_;
    bool dirty_ = false;
    std::vector<std::string> historyLabels_;
    std::vector<Project> historyStates_;
    std::size_t historyPosition_ = 0;
    std::vector<Observer> observers_;
    void notify();
};

struct BuildInfo {
    std::string version;
    std::string commit;
    std::string buildTime;
    std::string buildType;
    // Stable feature names that agents can probe before relying on a workflow.
    std::vector<std::string> features;
};

std::string version();
BuildInfo buildInfo();
std::string orientationName(Orientation orientation);

} // namespace ice
