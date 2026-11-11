#include "ice_openroad/odb_reader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "odb/db.h"
#include "odb/dbShape.h"
#include "odb/geom.h"

namespace ice::openroad {
namespace {

constexpr std::size_t kMaxPreviewShapes = 250000;

double microns(int value, int dbu) { return dbu > 0 ? static_cast<double>(value) / static_cast<double>(dbu) : 0.0; }

Rect toRect(const odb::Rect &rect, int dbu) {
    Rect result;
    result.x = microns(rect.xMin(), dbu);
    result.y = microns(rect.yMin(), dbu);
    result.width = microns(rect.xMax() - rect.xMin(), dbu);
    result.height = microns(rect.yMax() - rect.yMin(), dbu);
    return result;
}

std::string layerName(odb::dbTechLayer *layer) {
    if (!layer)
        return {};
    return layer->getName();
}

int layerNumber(odb::dbTechLayer *layer) {
    if (!layer)
        return 0;
    return layer->getNumber();
}

void addBoxShape(std::vector<PreviewShape> &shapes, odb::dbTechLayer *layer, const odb::Rect &box, int dbu,
                 bool &truncated) {
    if (shapes.size() >= kMaxPreviewShapes) {
        truncated = true;
        return;
    }
    PreviewShape shape;
    shape.layer = layerName(layer);
    shape.layer_number = layerNumber(layer);
    shape.kind = "polygon";
    shape.points = {{microns(box.xMin(), dbu), microns(box.yMin(), dbu)},
                    {microns(box.xMax(), dbu), microns(box.yMin(), dbu)},
                    {microns(box.xMax(), dbu), microns(box.yMax(), dbu)},
                    {microns(box.xMin(), dbu), microns(box.yMax(), dbu)}};
    shapes.push_back(std::move(shape));
}

void addViaShape(std::vector<PreviewShape> &shapes, const odb::dbShape &via, int dbu, bool &truncated) {
    std::vector<odb::dbShape> boxes;
    odb::dbShape::getViaBoxes(via, boxes);
    for (const auto &box : boxes)
        addBoxShape(shapes, box.getTechLayer(), box.getBox(), dbu, truncated);
}

void addWireShapes(std::vector<PreviewShape> &shapes, odb::dbWire *wire, int dbu, bool &truncated) {
    if (!wire)
        return;
    odb::dbWireShapeItr iterator;
    iterator.begin(wire);
    odb::dbShape shape;
    while (iterator.next(shape)) {
        if (shapes.size() >= kMaxPreviewShapes) {
            truncated = true;
            return;
        }
        if (shape.isVia()) {
            addViaShape(shapes, shape, dbu, truncated);
            continue;
        }
        if (shape.isSegment() || shape.isViaBox())
            addBoxShape(shapes, shape.getTechLayer(), shape.getBox(), dbu, truncated);
    }
}

void addSpecialWireShapes(std::vector<PreviewShape> &shapes, odb::dbNet *net, int dbu, bool &truncated) {
    if (!net || !net->isSpecial())
        return;
    for (odb::dbSWire *swire : net->getSWires()) {
        for (odb::dbSBox *box : swire->getWires()) {
            if (shapes.size() >= kMaxPreviewShapes) {
                truncated = true;
                return;
            }
            if (box->isVia()) {
                std::vector<odb::dbShape> via_boxes;
                box->getViaBoxes(via_boxes);
                for (const auto &via_box : via_boxes)
                    addBoxShape(shapes, via_box.getTechLayer(), via_box.getBox(), dbu, truncated);
                continue;
            }
            addBoxShape(shapes, box->getTechLayer(), box->getBox(), dbu, truncated);
        }
    }
}

void addPort(odb::dbBTerm *term, int dbu, std::vector<PreviewPort> &ports) {
    for (odb::dbBPin *pin : term->getBPins()) {
        for (odb::dbBox *box : pin->getBoxes()) {
            PreviewPort port;
            port.name = term->getName();
            if (box->isVia()) {
                std::vector<odb::dbShape> via_boxes;
                box->getViaBoxes(via_boxes);
                if (via_boxes.empty())
                    continue;
                port.layer = layerName(via_boxes.front().getTechLayer());
                port.layer_number = layerNumber(via_boxes.front().getTechLayer());
                port.rectangle = toRect(via_boxes.front().getBox(), dbu);
            } else {
                port.layer = layerName(box->getTechLayer());
                port.layer_number = layerNumber(box->getTechLayer());
                port.rectangle = toRect(box->getBox(), dbu);
            }
            if (port.rectangle.width <= 0.0 && port.rectangle.height <= 0.0)
                continue;
            ports.push_back(std::move(port));
        }
    }
}

void updateRange(HeatMapData &map) {
    map.minimum = std::numeric_limits<double>::max();
    map.maximum = std::numeric_limits<double>::lowest();
    bool any = false;
    for (const double value : map.values) {
        map.minimum = std::min(map.minimum, value);
        map.maximum = std::max(map.maximum, value);
        any = true;
    }
    if (!any) {
        map.minimum = 0.0;
        map.maximum = 0.0;
    }
}

} // namespace

struct OdbReader::Impl {
    std::unique_ptr<odb::dbDatabase, void (*)(odb::dbDatabase *)> db{nullptr, odb::dbDatabase::destroy};
    std::filesystem::path path;

    odb::dbBlock *block() const {
        if (!db)
            return nullptr;
        auto *chip = db->getChip();
        return chip ? chip->getBlock() : nullptr;
    }

    odb::dbTech *tech() const { return db ? db->getTech() : nullptr; }

    int dbu() const {
        auto *design = block();
        return design ? design->getDbUnitsPerMicron() : 0;
    }
};

OdbReader::OdbReader() : impl_(std::make_unique<Impl>()) {}

OdbReader::~OdbReader() = default;

bool OdbReader::open(const std::filesystem::path &path, std::string &error) {
    close();
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status) || status) {
        error = "OpenDB file does not exist: " + path.string();
        return false;
    }
    auto database = std::unique_ptr<odb::dbDatabase, void (*)(odb::dbDatabase *)>(odb::dbDatabase::create(),
                                                                                  odb::dbDatabase::destroy);
    if (!database) {
        error = "Unable to create an OpenDB database";
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "Unable to open OpenDB file: " + path.string();
            return false;
        }
        database->read(input);
        if (input.bad() || (input.fail() && !input.eof())) {
            error = "Unable to read OpenDB file: " + path.string();
            return false;
        }
    } catch (const std::exception &exception) {
        error = "Unable to read OpenDB file: " + std::string(exception.what());
        return false;
    }
    if (!database->getChip() || !database->getChip()->getBlock()) {
        error = "OpenDB file has no design block: " + path.string();
        return false;
    }
    impl_->db = std::move(database);
    impl_->path = path;
    return true;
}

void OdbReader::close() {
    impl_->db.reset();
    impl_->path.clear();
}

bool OdbReader::isOpen() const { return impl_->db != nullptr; }

OdbSummary OdbReader::summary() const {
    OdbSummary result;
    auto *block = impl_->block();
    if (!block)
        return result;
    const int dbu = impl_->dbu();
    result.dbu_per_micron = dbu;
    result.design_name = block->getName();
    if (auto *chip = impl_->db->getChip())
        result.top_cell = chip->getName();
    if (result.top_cell.empty())
        result.top_cell = result.design_name;
    result.die_area = toRect(block->getDieArea(), dbu);
    result.core_area = toRect(block->getCoreArea(), dbu);
    result.instance_count = block->getInsts().size();
    result.net_count = block->getNets().size();
    result.pin_count = block->getBTerms().size();
    result.row_count = block->getRows().size();
    for (auto *net : block->getNets()) {
        if (net->isSpecial())
            ++result.special_net_count;
    }
    return result;
}

OdbPreview OdbReader::preview(int heatmap_bins) const {
    OdbPreview result;
    result.summary = summary();
    auto *block = impl_->block();
    if (!block)
        return result;
    const int dbu = impl_->dbu();
    result.bounds = result.summary.die_area;

    // Special (power/ground) shapes first so signal routing stays visible.
    for (auto *net : block->getNets())
        addSpecialWireShapes(result.shapes, net, dbu, result.truncated);

    // Standard-cell and macro outlines. The renderer paints these as planned
    // devices, so they remain distinguishable from routed geometry.
    for (auto *inst : block->getInsts()) {
        if (!inst->getPlacementStatus().isPlaced())
            continue;
        PreviewInstance preview;
        preview.name = inst->getName();
        if (auto *master = inst->getMaster()) {
            preview.master = master->getName();
            if (auto *bbox = inst->getBBox())
                preview.rectangle = toRect(bbox->getBox(), dbu);
        }
        preview.orientation = inst->getOrient().getString();
        if (preview.rectangle.width <= 0.0 && preview.rectangle.height <= 0.0)
            continue;
        result.instances.push_back(std::move(preview));
    }

    // Routed signal wires.
    for (auto *net : block->getNets()) {
        if (net->isSpecial())
            continue;
        addWireShapes(result.shapes, net->getWire(), dbu, result.truncated);
    }

    for (auto *term : block->getBTerms())
        addPort(term, dbu, result.ports);

    if (heatmap_bins > 0) {
        result.placement_density = placementDensity(heatmap_bins, heatmap_bins);
        result.routing_congestion = routingCongestion(heatmap_bins, heatmap_bins);
    }
    return result;
}

std::optional<HeatMapData> OdbReader::placementDensity(int bins_x, int bins_y) const {
    auto *block = impl_->block();
    if (!block || bins_x <= 0 || bins_y <= 0)
        return std::nullopt;
    const int dbu = impl_->dbu();
    const odb::Rect core = block->getCoreArea();
    if (core.dx() <= 0 || core.dy() <= 0)
        return std::nullopt;

    HeatMapData map;
    map.kind = HeatMapKind::placement_density;
    map.columns = bins_x;
    map.rows = bins_y;
    map.bounds = toRect(core, dbu);
    map.values.assign(static_cast<std::size_t>(bins_x) * static_cast<std::size_t>(bins_y), 0.0);

    const double bin_width = static_cast<double>(core.dx()) / bins_x;
    const double bin_height = static_cast<double>(core.dy()) / bins_y;
    const double bin_area = bin_width * bin_height;
    if (bin_area <= 0.0)
        return std::nullopt;

    for (auto *inst : block->getInsts()) {
        if (!inst->getPlacementStatus().isPlaced())
            continue;
        auto *master = inst->getMaster();
        if (!master)
            continue;
        if (master->isFiller())
            continue;
        if (master->getType() == odb::dbMasterType::CORE_WELLTAP || master->isEndCap())
            continue;
        if (master->isPad() || master->isCover())
            continue;
        auto *bbox = inst->getBBox();
        if (!bbox)
            continue;
        const odb::Rect box = bbox->getBox();
        const int x0 = std::clamp(static_cast<int>(std::floor((box.xMin() - core.xMin()) / bin_width)), 0, bins_x - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((box.xMax() - core.xMin()) / bin_width)), 0, bins_x);
        const int y0 = std::clamp(static_cast<int>(std::floor((box.yMin() - core.yMin()) / bin_height)), 0, bins_y - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil((box.yMax() - core.yMin()) / bin_height)), 0, bins_y);
        for (int y = y0; y < y1; ++y) {
            const int overlap_y =
                std::max(0, std::min(box.yMax(), core.yMin() + static_cast<int>((y + 1) * bin_height)) -
                                std::max(box.yMin(), core.yMin() + static_cast<int>(y * bin_height)));
            for (int x = x0; x < x1; ++x) {
                const int overlap_x =
                    std::max(0, std::min(box.xMax(), core.xMin() + static_cast<int>((x + 1) * bin_width)) -
                                    std::max(box.xMin(), core.xMin() + static_cast<int>(x * bin_width)));
                const double overlap = static_cast<double>(overlap_x) * static_cast<double>(overlap_y);
                if (overlap <= 0.0)
                    continue;
                map.values[static_cast<std::size_t>(y) * bins_x + x] += 100.0 * overlap / bin_area;
            }
        }
    }
    for (double &value : map.values)
        value = std::min(100.0, value);
    updateRange(map);
    return map;
}

std::optional<HeatMapData> OdbReader::routingCongestion(int bins_x, int bins_y) const {
    auto *block = impl_->block();
    if (!block || bins_x <= 0 || bins_y <= 0)
        return std::nullopt;
    const int dbu = impl_->dbu();
    odb::Rect bounds = block->getCoreArea();
    if (bounds.dx() <= 0 || bounds.dy() <= 0)
        bounds = block->getDieArea();
    if (bounds.dx() <= 0 || bounds.dy() <= 0)
        return std::nullopt;

    HeatMapData map;
    map.kind = HeatMapKind::routing_congestion;
    map.columns = bins_x;
    map.rows = bins_y;
    map.bounds = toRect(bounds, dbu);
    map.values.assign(static_cast<std::size_t>(bins_x) * static_cast<std::size_t>(bins_y), 0.0);

    const double bin_width = static_cast<double>(bounds.dx()) / bins_x;
    const double bin_height = static_cast<double>(bounds.dy()) / bins_y;
    const double bin_area = bin_width * bin_height;
    if (bin_area <= 0.0)
        return std::nullopt;

    const auto accumulate = [&](const odb::Rect &box) {
        const int x0 =
            std::clamp(static_cast<int>(std::floor((box.xMin() - bounds.xMin()) / bin_width)), 0, bins_x - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((box.xMax() - bounds.xMin()) / bin_width)), 0, bins_x);
        const int y0 =
            std::clamp(static_cast<int>(std::floor((box.yMin() - bounds.yMin()) / bin_height)), 0, bins_y - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil((box.yMax() - bounds.yMin()) / bin_height)), 0, bins_y);
        for (int y = y0; y < y1; ++y) {
            const int overlap_y =
                std::max(0, std::min(box.yMax(), bounds.yMin() + static_cast<int>((y + 1) * bin_height)) -
                                std::max(box.yMin(), bounds.yMin() + static_cast<int>(y * bin_height)));
            for (int x = x0; x < x1; ++x) {
                const int overlap_x =
                    std::max(0, std::min(box.xMax(), bounds.xMin() + static_cast<int>((x + 1) * bin_width)) -
                                    std::max(box.xMin(), bounds.xMin() + static_cast<int>(x * bin_width)));
                const double overlap = static_cast<double>(overlap_x) * static_cast<double>(overlap_y);
                if (overlap > 0.0)
                    map.values[static_cast<std::size_t>(y) * bins_x + x] += 100.0 * overlap / bin_area;
            }
        }
    };

    const auto addNet = [&](odb::dbNet *net) {
        odb::dbWire *wire = net->getWire();
        if (!wire)
            return;
        odb::dbWireShapeItr iterator;
        iterator.begin(wire);
        odb::dbShape shape;
        while (iterator.next(shape)) {
            if (shape.isVia()) {
                std::vector<odb::dbShape> boxes;
                odb::dbShape::getViaBoxes(shape, boxes);
                for (const auto &box : boxes)
                    accumulate(box.getBox());
                continue;
            }
            if (shape.isSegment() || shape.isViaBox())
                accumulate(shape.getBox());
        }
    };
    for (auto *net : block->getNets()) {
        if (!net->isSpecial())
            addNet(net);
    }
    for (double &value : map.values)
        value = std::min(100.0, value);
    updateRange(map);
    return map;
}

} // namespace ice::openroad
