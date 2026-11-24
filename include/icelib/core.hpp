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
    // Bus width metadata for pins generated from HDL module ports. Primitive
    // and manually drawn symbols default to a scalar pin.
    int width = 1;
    bool widthKnown = true;
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

struct NetLabelAnchor {
    // The label follows the schematic branch between these two pins. The
    // fraction is measured from fromPin to toPin along the routed branch.
    ObjectId fromPin = 0;
    ObjectId toPin = 0;
    double fraction = 0.5;
    // False places the label on side A of the branch direction, true on B.
    bool sideB = false;
};

struct Net {
    ObjectId id = 0;
    std::string name;
    std::vector<ObjectId> pins;
    Coordinate x = 0;
    Coordinate y = 0;
    // Optional route-relative label anchor. x/y remain as a fallback for
    // labels that have not been dragged or whose anchor branch disappeared.
    std::optional<NetLabelAnchor> labelAnchor;
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
    // Original file path for text SPICE imported from disk. Relative
    // `.include`/`.lib` paths are resolved against this file.
    std::string spiceSourcePath;
    // True for a schematic authored as a SystemVerilog source cell. Source
    // cells participate in the project hierarchy and simulation selector but
    // are not drawn canvases or hierarchical blocks.
    bool systemVerilogSchematic = false;
    std::string systemVerilogText;
    bool isTextSource() const { return textSchematic || systemVerilogSchematic; }
    const std::string &sourceText() const { return systemVerilogSchematic ? systemVerilogText : spiceText; }
};

struct Layer {
    ObjectId id = 0;
    std::string name;
    int number = 0;
    bool visible = true;
    // Zero means this is a project-wide layer. Non-zero custom layers belong
    // only to that layout cell; PDK layers are shared by matching PDKs.
    ObjectId layout = 0;
    std::string pdk;
    // GDSII datatype/purpose. Layers with the same number but different
    // datatypes are separate entries so geometry and ports keep their exact
    // stream identity. The Layers panel groups them by number for display.
    int datatype = 0;
    // Empty color and pattern -1 mean the UI derives both from the layer
    // number. Explicit values let users style custom layers.
    std::string color;
    int pattern = -1;
    // Empty for shared/pdk-wide layers; otherwise only the named PDK stack
    // exposes this layer to its layout cells.
    std::string stack;
    // Minimum drawn width in database units (nanometres). Paths on this layer
    // use at least this width. Zero means no PDK minimum is known.
    Coordinate minimumWidth = 0;
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
    // Nominal path width, used when segmentWidths is empty and for legacy
    // files. Paths with per-segment widths keep the largest width here.
    Coordinate width = 0;
    // Width of each centerline segment (size centerline.size() - 1). Empty
    // means every segment uses `width`. Paths use this to keep each segment at
    // its layer's minimum width without splitting the path.
    std::vector<Coordinate> segmentWidths;
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
    // Non-zero when this path was generated by automatic routing for a
    // planned connection and should follow its endpoints.
    ObjectId routedConnection = 0;
    bool isPath() const { return centerline.size() >= 2 && widthForSegment(0) > 0; }
    bool isText() const { return !text.empty(); }
    ObjectId layerForSegment(std::size_t index) const {
        return index < segmentLayers.size() ? segmentLayers[index] : layer;
    }
    Coordinate widthForSegment(std::size_t index) const {
        return index < segmentWidths.size() ? segmentWidths[index] : width;
    }
};

enum class PortDirection { input, output, inout, power, ground };
enum class PortSide { left, right, top, bottom };

const char *portDirectionName(PortDirection direction);
const char *portSideName(PortSide side);
// Locale-independent floating-point parser. Parses a decimal number from the
// beginning of `text` and returns the number of consumed bytes. This is a
// portable replacement for std::from_chars(double), which libc++ does not
// implement.
std::optional<double> parseDoublePrefix(const std::string &text, std::size_t &consumed);

std::optional<PortDirection> parsePortDirection(const std::string &value);
std::optional<PortSide> parsePortSide(const std::string &value);

struct LayoutNet {
    ObjectId id = 0;
    std::string name;
};

// Ports are intrinsic ICE annotations with a required process-layer
// association. The layer is persisted in `.icelay` files and drives routing,
// DRC/LVS matching, and interchange.
struct Port {
    ObjectId id = 0;
    std::string name;
    // Required associated layout layer. Ports are validated and saved with it.
    ObjectId layer = 0;
    Rect rectangle;
    PortDirection direction = PortDirection::inout;
    PortSide side = PortSide::left;
    // LayoutNet id, or zero when the port is intentionally unassigned.
    ObjectId net = 0;
    std::string label;
    bool visible = true;
    bool locked = false;
    // True for automatically promoted device terminals. Internal ports are
    // useful for routing and connectivity but are not top-level LVS ports.
    bool internal = false;
    // Set when this port is promoted from a child layout instance.
    ObjectId sourceInstance = 0;
    ObjectId sourcePort = 0;
    // Set when this port replaces a schematic source or ground instance.
    ObjectId schematicInstance = 0;
};

struct LayoutInstance {
    ObjectId id = 0;
    ObjectId cell = 0;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
    // Schematic instance this placed device was created from, when known.
    ObjectId schematicInstance = 0;
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
    // Optional device identity. Procedural layout scripts use this to keep
    // the model, parameters, and terminal nets of a placed PCell even when its
    // geometry is flattened into the generated cell.
    std::string device;
    std::string model;
    std::vector<Parameter> parameters;
    // Device terminal name (for example "G") to layout net id.
    std::map<std::string, ObjectId> terminals;
    // False when this entry only preserves device identity for LVS while the
    // real geometry is already stored in the cell.
    bool placeholder = true;
};

// A straight logical connection between two planned devices, placed layout
// instances, or top-level layout ports. The net points at a LayoutNet in the
// same cell. For a placed instance endpoint, fromPort/toPort identify the child
// layout port; the fromPin/toPin names preserve the schematic pin that created
// the connection.
struct PlannedConnection {
    ObjectId id = 0;
    ObjectId from = 0;
    ObjectId to = 0;
    ObjectId net = 0;
    ObjectId fromPort = 0;
    ObjectId toPort = 0;
    std::string fromPin;
    std::string toPin;
    // Optional border overrides for automatically routed endpoints. When
    // unset, the router uses the port's narrowest side facing the other port.
    std::optional<PortSide> fromSide;
    std::optional<PortSide> toSide;
    // True once a physical path has been created for this logical connection.
    bool routed = false;
};

struct LayoutReference {
    // Path to the external stream, relative to the project root when possible.
    std::string path;
    // "gds" or "oasis".
    std::string format;
    // Top cell to display. Empty lets the reader choose the sole top cell.
    std::string topCell;
};

struct LayoutCell {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    // Selected PDK stack variant. Empty means the package default or a
    // stack-less PDK.
    std::string stack;
    // True for cells imported from a read-only device library.
    bool library = false;
    // True for imported cells that should be visible in the layout tree but
    // cannot be edited. Unlike `library`, this does not tie the cell to the
    // device-library browser.
    bool readOnly = false;
    // When set, the cell is a read-only reference to an external GDS/OASIS
    // stream instead of native geometry stored in this project.
    std::optional<LayoutReference> reference;
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
    // Schematic cell this layout was created from or manually linked to.
    // LVS prefers this link over name inference and the GUI uses it for
    // cross-probing.
    ObjectId schematicCell = 0;
};

// A layout whose geometry is produced by a Python script. The generated
// static layout lives in generatedCell; regenerating the script replaces that
// cell's contents while keeping the procedural layout as the source of truth.
struct ProceduralLayout {
    ObjectId id = 0;
    std::string name;
    std::string pdk = genericPdkId;
    std::string stack;
    std::string script;
    ObjectId generatedCell = 0;
    // User-editable parameters exposed to the script through layout.parameter().
    std::vector<Parameter> parameters;
    // Hash of the script used for the last successful generation. Empty means
    // the generated cell has never been produced.
    std::string generatedScriptHash;
    // Hash of the parameter values used for the last successful generation.
    std::string generatedParameterHash;
    // Provenance for the last successful generation.
    std::string generatedPdkVersion;
    std::string generatedOutputHash;
    // Structured runner metadata from the last successful generation. These
    // are JSON objects: placement quality, declared plan, and agent review.
    std::string generatedLayoutQuality;
    std::string generatedLayoutPlan;
    std::string generatedLayoutReview;
};

enum class ImplementationStage { created, elaboration, synthesis, floorplan, placement, cts, routing, final };

enum class ImplementationStatus { pending, running, succeeded, failed, cancelled };

enum class ImplementationStageStatus { pending, running, succeeded, failed, skipped, cancelled };

struct ImplementationMacroPlacement {
    std::string cell;
    Coordinate x = 0;
    Coordinate y = 0;
    std::string orientation = "r0";
};

// User-facing configuration for one RTL-to-GDS run. The same structure feeds
// the direct OpenROAD backend and, later, alternative backends such as
// LibreLane. It must stay free of backend-specific file formats.
struct ImplementationConfig {
    std::string name;
    std::string top;
    std::string pdk = genericPdkId;
    std::string stack;
    std::string standardCellLibrary;
    std::string corner = "tt";
    std::vector<std::filesystem::path> sources;
    std::vector<ObjectId> sourceCells;
    std::filesystem::path sdc;

    // Floorplan policy. Zero/empty values mean "use the platform default".
    std::optional<Rect> dieArea;
    std::optional<Rect> coreArea;
    double utilization = 0.5;
    double aspectRatio = 1.0;
    std::string site;
    std::vector<std::string> ioHorizontalLayers;
    std::vector<std::string> ioVerticalLayers;
    std::vector<ImplementationMacroPlacement> macros;

    // Placement/CTS/routing policy.
    double placementDensity = 0.0;
    bool timingDriven = false;
    bool routabilityDriven = false;
    bool ctsEnabled = true;
    std::string ctsRootBuffer;
    std::vector<std::string> ctsBuffers;
    std::string routingMinLayer;
    std::string routingMaxLayer;
    bool antennaRepair = true;

    // Execution policy.
    std::string backend = "openroad";
    int threads = 1;
    std::uint32_t seed = 1;
    int timeoutSeconds = 3600;
};

struct ImplementationStageState {
    ImplementationStage stage = ImplementationStage::created;
    ImplementationStageStatus status = ImplementationStageStatus::pending;
    std::string startedAt;
    std::string finishedAt;
    std::string error;
    std::vector<std::string> artifacts;
};

// A persistent RTL-to-GDS run. Heavy stage artifacts live under
// build/impl/<run>/ and are referenced by relative path; the run manifest
// itself is stored in impl/<run>.iceimpl.
struct ImplementationRun {
    ObjectId id = 0;
    std::string name;
    ImplementationConfig config;
    ImplementationStage currentStage = ImplementationStage::created;
    ImplementationStatus status = ImplementationStatus::pending;
    std::map<ImplementationStage, ImplementationStageState> stages;
    std::string directory;
    std::string configHash;
    std::map<std::string, std::string> toolVersions;
    std::map<std::string, double> metrics;
    std::vector<std::string> artifacts;
    ObjectId resultLayout = 0;
    std::string lastError;
    std::string createdAt;
    std::string updatedAt;
};

// A derived trace defined by a waveform expression, for example
// "out - in", "db(out)", or "derivative(i(v1))". Expressions only affect how
// stored runs are displayed; the simulator output is never rewritten.
struct SimulationDerivedTrace {
    std::string name;
    std::string expression;
};

// One additional stored run drawn on the same graph. `runId` is the identifier
// written by the simulation store; the label defaults to the run variant.
struct SimulationOverlayRun {
    std::string runId;
    std::string label;
};

// Persistent point annotation placed by the user on a plot.
struct SimulationGraphMarker {
    std::string label;
    std::string signal;
    double x = 0.0;
    double y = 0.0;
    bool hasY = false;
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
    // Display transform: "linear", "db", or "phase".
    std::string view = "linear";
    // Frequency-domain view of the selected traces.
    bool fft = false;
    std::string fftWindow = "hann";
    int fftPoints = 0; // 0 selects the next power of two.
    bool showMeasurements = true;
    std::vector<SimulationDerivedTrace> derivedTraces;
    std::vector<SimulationOverlayRun> overlays;
    std::vector<SimulationGraphMarker> markers;
};

// One process/temperature/supply corner for a simulation experiment. Empty
// modelSection and parameters keep the project/PDK defaults; sourceOverrides
// use ngspice source names such as "V1".
struct SimulationCorner {
    std::string name = "tt";
    std::string modelSection;
    double temperature = 27.0;
    std::map<std::string, double> sourceOverrides;
    std::map<std::string, double> parameters;
};

// One parameter sweep axis. Values are expanded as a Cartesian product.
struct SimulationSweep {
    std::string parameter;
    std::vector<double> values;
};

// Deterministic Monte Carlo variation on top of the nominal run or corners.
struct SimulationMonteCarloOptions {
    bool enabled = false;
    int samples = 20;
    double temperatureSigma = 0.0;
    double supplySigmaPercent = 0.0;
    std::string supplySource = "V1";
    double supplyNominal = 1.0;
    std::map<std::string, double> parameterSigmaPercent;
    std::uint32_t seed = 1;
};

// Declarative post-simulation measurement. Supported metrics are gain,
// bandwidth, phase_margin, slew_rate, settling_time, and power.
struct SimulationMeasurementSpec {
    std::string name;
    std::string metric;
    std::string signal;
    std::string input;
    double voltage = 0.0;
    std::optional<double> threshold;
    double tolerance = 0.02;
};

struct SimulationItem {
    ObjectId id = 0;
    std::string name;
    ObjectId schematic = 0;
    std::string simulator = "ngspice";
    std::string analysis = "transient";
    // Transient analysis window. `start` is optional (zero keeps the ngspice
    // default) and `stop`/`step` are also reused by DC sweeps unless the
    // dedicated DC fields are set.
    double start = 0.0;
    double stop = 1.0;
    double step = 0.01;
    bool use_initial_conditions = false;
    std::string directives;
    // Process/temperature/supply overrides applied to every run.
    double temperature = 27.0;
    std::string modelSection;
    std::map<std::string, double> parameters;
    std::map<std::string, double> sourceOverrides;
    // DC sweep.
    std::string dcSource;
    double dcStart = 0.0;
    double dcStop = 1.0;
    double dcStep = 0.01;
    // AC sweep.
    std::string acVariation = "dec";
    int acPoints = 10;
    double acStart = 1.0;
    double acStop = 1e6;
    // Noise analysis.
    std::string noiseOutput;
    std::string noiseInput;
    std::string noiseVariation = "dec";
    int noisePoints = 10;
    double noiseStart = 1.0;
    double noiseStop = 1e6;
    // Transfer-function analysis.
    std::string tfOutput;
    std::string tfSource;
    // Comma-separated signals requested from the simulator. Axis selections in
    // each graph reference entries in this list, or the special "time" value.
    std::string captureSignals;
    // Optional experiment configuration. Empty corners/sweeps with Monte Carlo
    // disabled is the normal single-run workflow.
    std::vector<SimulationCorner> corners;
    std::vector<SimulationSweep> sweeps;
    SimulationMonteCarloOptions monteCarlo;
    std::vector<SimulationMeasurementSpec> measurements;
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
    // Package names this project explicitly trusts to execute package-owned
    // code (for example PDK procedural-layout device generators). This is a
    // project-local policy decision; it never overrides an explicit
    // ICE_ALLOW_PACKAGE_CODE=0 kill switch.
    std::vector<std::string> trustedPackages;
    std::map<ObjectId, SchematicCell> schematicCells;
    std::map<ObjectId, Layer> layers;
    std::map<ObjectId, LayoutCell> layoutCells;
    std::map<ObjectId, ProceduralLayout> proceduralLayouts;
    std::map<ObjectId, ImplementationRun> implementationRuns;
    std::map<ObjectId, SimulationItem> simulations;
    std::map<ObjectId, ProjectReport> reports;

    // Returns a fresh object id. Ids are monotonic even when several are
    // allocated before the objects are inserted (for example a copied symbol
    // together with its ports).
    ObjectId nextId();
    Coordinate snap(Coordinate value) const;
    std::vector<Diagnostic> validate() const;
    bool trustsPackage(const std::string &package) const;
    std::string serialize() const;
    static Project deserialize(const std::string &text);
    // Saves/loads an ICE project directory. `path` may be the project root or
    // its project.icepro file. The project file contains only project metadata;
    // schematics, layouts, and simulations are stored in sch/, lay/, and sim/.
    void save(const std::filesystem::path &path) const;
    // Saves only the file for one schematic, layout, or procedural cell. The
    // project must already have been saved once so its root is known. Returns
    // the file that was written.
    std::filesystem::path saveCellFile(ObjectId id) const;
    static Project load(const std::filesystem::path &path);
    static Project load(const std::filesystem::path &path, const ProjectLoadOptions &options,
                        ProjectLoadReport *report = nullptr);

    // Revision of the split files as they existed when this project was loaded
    // or last saved. Empty for a project that has never been loaded or saved.
    const std::string &sourceRevision() const noexcept { return sourceRevision_; }
    // Adopt the disk fingerprint (root + revision) of another project. Used when
    // a session restores an in-memory history entry (undo/redo) so that the next
    // save is still compared against the revision that is actually on disk.
    void adoptSourceRevisionFrom(const Project &other);

  private:
    mutable std::filesystem::path sourceRoot_;
    mutable std::string sourceRevision_;
    // Highest id handed out by nextId(). Kept as a high-water mark so that ids
    // allocated in a batch, before the objects exist, stay unique.
    ObjectId lastIssuedId_ = 0;
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
    // Replace the session contents. Pass markDirty=false when the project was
    // just read from disk (reload/recover), so the session is not left with a
    // pending save that would rewrite files nobody changed.
    void replaceProject(Project project, bool markDirty = true);
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

// Resolve the directory that holds project.icepro. Accepts a project file, a
// bare file name, or the project directory itself; a bare file name resolves to
// the current directory instead of an empty path.
std::filesystem::path projectRootDirectory(const std::filesystem::path &path);

} // namespace ice
