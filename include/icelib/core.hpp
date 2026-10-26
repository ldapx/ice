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
};

struct Layer {
    ObjectId id = 0;
    std::string name;
    int number = 0;
    bool visible = true;
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
};

struct Port {
    ObjectId id = 0;
    std::string name;
    ObjectId layer = 0;
    Rect rectangle;
};

struct LayoutInstance {
    ObjectId id = 0;
    ObjectId cell = 0;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
};

struct LayoutCell {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    // True for cells imported from a read-only device library.
    bool library = false;
    std::map<ObjectId, Shape> shapes;
    std::map<ObjectId, Port> ports;
    std::map<ObjectId, LayoutInstance> instances;
};

struct SimulationGraph {
    ObjectId id = 0;
    std::string name;
    // Axis selections are signal names from the simulation's captured signal
    // list. The literal "time" selects the simulator's time vector. X selects
    // one signal; Y can contain multiple signals sharing the same X axis.
    std::string xAxis = "time";
    std::vector<std::string> ySignals;
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

class Project {
  public:
    static constexpr int schemaVersion = 1;

    std::string identity;
    std::string name;
    DatabaseUnits units;
    Coordinate grid = 1;
    std::vector<std::string> packageDependencies;
    std::map<ObjectId, SchematicCell> schematicCells;
    std::map<ObjectId, Layer> layers;
    std::map<ObjectId, LayoutCell> layoutCells;
    std::map<ObjectId, SimulationItem> simulations;

    ObjectId nextId();
    Coordinate snap(Coordinate value) const;
    std::vector<Diagnostic> validate() const;
    std::string serialize() const;
    static Project deserialize(const std::string &text);
    void save(const std::filesystem::path &path) const;
    static Project load(const std::filesystem::path &path);
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
    std::vector<Observer> observers_;
    void notify();
};

std::string version();
std::string orientationName(Orientation orientation);

} // namespace ice
