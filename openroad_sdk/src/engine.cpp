#include "ice_openroad/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "ant/AntennaChecker.hh"
#include "bazel/tcl_library_init.h"
#include "cts/TritonCTS.h"
#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "dpl/Opendp.h"
#include "drt/TritonRoute.h"
#include "est/EstimateParasitics.h"
#include "gpl/Replace.h"
#include "grt/GlobalRouter.h"
#include "ifp/InitFloorplan.hh"
#include "odb/db.h"
#include "ord/Design.h"
#include "ord/InitOpenRoad.hh"
#include "ord/OpenRoad.hh"
#include "ord/Tech.h"
#include "ppl/IOPlacer.h"
#include "rcx/ext.h"
#include "rcx/ext_options.h"
#include "rsz/Resizer.hh"
#include "sta/Clock.hh"
#include "sta/MinMax.hh"
#include "sta/PowerClass.hh"
#include "sta/Sdc.hh"
#include "sta/Sta.hh"
#include "tcl.h"
#include "utl/Logger.h"
#include "web/core.h"

namespace ice::openroad {
namespace {

std::string tclQuote(const std::string &text) {
    Tcl_Obj *list = Tcl_NewListObj(0, nullptr);
    Tcl_IncrRefCount(list);
    Tcl_ListObjAppendElement(nullptr, list, Tcl_NewStringObj(text.c_str(), -1));
    const std::string result = Tcl_GetString(list);
    Tcl_DecrRefCount(list);
    return result;
}

bool writeText(const std::filesystem::path &path, const std::string &text, std::string &error) {
    std::error_code status;
    const auto parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, status);
    if (status) {
        error = "Unable to create " + parent.string() + ": " + status.message();
        return false;
    }
    std::ofstream output(path);
    if (!output) {
        error = "Unable to write " + path.string();
        return false;
    }
    output << text;
    return true;
}

std::string jsonNumber(double value) {
    std::ostringstream output;
    output.precision(15);
    output << value;
    return output.str();
}

odb::Rect toRect(const Rect &rect, int dbu) {
    const auto x = static_cast<int>(std::llround(rect.x * dbu));
    const auto y = static_cast<int>(std::llround(rect.y * dbu));
    const auto x2 = static_cast<int>(std::llround((rect.x + rect.width) * dbu));
    const auto y2 = static_cast<int>(std::llround((rect.y + rect.height) * dbu));
    return odb::Rect(x, y, x2, y2);
}

} // namespace

struct Engine::Impl {
    Tcl_Interp *interp = nullptr;
    std::unique_ptr<ord::Tech> tech;
    std::unique_ptr<ord::Design> design;
    ord::OpenRoad *openroad = nullptr;
    std::filesystem::path log_file;
    std::filesystem::path metrics_file;
    int threads = 1;

    bool eval(const std::string &command, std::string &error) {
        if (!interp) {
            error = "Tcl interpreter is not initialized";
            return false;
        }
        if (Tcl_Eval(interp, command.c_str()) != TCL_OK) {
            error = Tcl_GetStringResult(interp);
            return false;
        }
        return true;
    }

    bool source(const std::filesystem::path &path, std::string &error) {
        if (path.empty())
            return true;
        if (!std::filesystem::is_regular_file(path)) {
            error = "Tcl hook does not exist: " + path.string();
            return false;
        }
        if (Tcl_EvalFile(interp, path.c_str()) != TCL_OK) {
            error = "Tcl hook " + path.string() + " failed: " + Tcl_GetStringResult(interp);
            return false;
        }
        return true;
    }

    bool loadStageInput(const StageJob &job, std::string &error) {
        if (job.input_db.empty() || !std::filesystem::is_regular_file(job.input_db)) {
            error = "Input ODB does not exist: " + job.input_db.string();
            return false;
        }
        openroad->readDb(job.input_db.string().c_str());
        auto *sta = openroad->getSta();
        for (const auto &liberty : job.liberty) {
            if (!std::filesystem::is_regular_file(liberty)) {
                error = "Liberty file does not exist: " + liberty.string();
                return false;
            }
            sta->readLiberty(liberty.string(), sta->cmdScene(), sta::MinMaxAll::all(), false);
        }
        if (!job.sdc.empty()) {
            if (!std::filesystem::is_regular_file(job.sdc)) {
                error = "SDC file does not exist: " + job.sdc.string();
                return false;
            }
            if (!eval("read_sdc " + tclQuote(job.sdc.string()), error))
                return false;
        }
        return true;
    }

    void collectTimingMetrics(StageResult &result) {
        auto *sta = openroad->getSta();
        if (!sta)
            return;
        try {
            result.metrics["timing__setup__ws"] = static_cast<double>(sta->worstSlack(sta::MinMax::max()));
            result.metrics["timing__setup__tns"] = static_cast<double>(sta->totalNegativeSlack(sta::MinMax::max()));
            result.metrics["timing__hold__ws"] = static_cast<double>(sta->worstSlack(sta::MinMax::min()));
            result.metrics["timing__hold__tns"] = static_cast<double>(sta->totalNegativeSlack(sta::MinMax::min()));
            result.metrics["timing__worst_slack"] = result.metrics["timing__setup__ws"];
            result.metrics["timing__tns"] = result.metrics["timing__setup__tns"];
        } catch (const std::exception &) {
            // Timing metrics are advisory; an unconstrained design can leave
            // no path to report.
        }
    }

    void collectPowerMetrics(StageResult &result) {
        auto *sta = openroad->getSta();
        if (!sta || !sta->cmdScene())
            return;
        try {
            sta::PowerResult total;
            sta::PowerResult sequential;
            sta::PowerResult combinational;
            sta::PowerResult clock;
            sta::PowerResult macro;
            sta::PowerResult pad;
            sta->power(sta->cmdScene(), total, sequential, combinational, clock, macro, pad);
            result.metrics["power__total"] = static_cast<double>(total.total());
        } catch (const std::exception &) {
            // Power is advisory in the migration phases.
        }
    }

    void collectMetrics(StageResult &result) {
        auto *db = openroad->getDb();
        auto *block = db->getChip() ? db->getChip()->getBlock() : nullptr;
        if (!block)
            return;
        const int dbu = block->getDbUnitsPerMicron();
        const double dbu_area = static_cast<double>(dbu) * static_cast<double>(dbu);
        result.metrics["design__die__area"] = static_cast<double>(block->getDieArea().area()) / dbu_area;
        result.metrics["design__core__area"] = static_cast<double>(block->getCoreArea().area()) / dbu_area;
        result.metrics["design__instance__count"] =
            static_cast<double>(std::distance(block->getInsts().begin(), block->getInsts().end()));
        result.metrics["design__bterm__count"] =
            static_cast<double>(std::distance(block->getBTerms().begin(), block->getBTerms().end()));
        result.metrics["design__net__count"] =
            static_cast<double>(std::distance(block->getNets().begin(), block->getNets().end()));
        double instance_area = 0.0;
        for (auto *inst : block->getInsts()) {
            if (auto *master = inst->getMaster()) {
                instance_area +=
                    static_cast<double>(master->getWidth()) * static_cast<double>(master->getHeight()) / dbu_area;
            }
        }
        result.metrics["design__instance__area"] = instance_area;
    }

    void collectRouteMetrics(StageResult &result) {
        auto *db = openroad->getDb();
        auto *block = db->getChip() ? db->getChip()->getBlock() : nullptr;
        if (!block)
            return;
        const int dbu = block->getDbUnitsPerMicron();
        double wirelength = 0.0;
        std::uint64_t vias = 0;
        for (auto *net : block->getNets()) {
            if (net->isSpecial())
                continue;
            auto *wire = net->getWire();
            if (!wire)
                continue;
            odb::dbWireShapeItr iterator;
            iterator.begin(wire);
            odb::dbShape shape;
            while (iterator.next(shape)) {
                if (shape.isVia()) {
                    ++vias;
                    continue;
                }
                if (shape.isSegment()) {
                    const auto box = shape.getBox();
                    int length = std::max(box.dx(), box.dy());
                    if (auto *layer = shape.getTechLayer())
                        length = std::max(0, length - static_cast<int>(layer->getWidth()));
                    wirelength += static_cast<double>(length) / dbu;
                }
            }
        }
        result.metrics["route__wirelength"] = wirelength;
        result.metrics["route__vias"] = static_cast<double>(vias);
    }

    bool writeStageOutputs(const StageJob &job, StageResult &result, std::string &error, bool write_verilog = false) {
        auto *db = openroad->getDb();
        auto *block = db->getChip() ? db->getChip()->getBlock() : nullptr;
        if (!block) {
            error = "No design block";
            return false;
        }
        if (!job.output_db.empty()) {
            std::error_code status;
            const auto parent = job.output_db.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            openroad->writeDb(job.output_db.string().c_str());
            result.artifacts.push_back(job.output_db);
        }
        if (!job.output_def.empty()) {
            std::error_code status;
            const auto parent = job.output_def.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            openroad->writeDef(job.output_def.string().c_str(), "5.8");
            result.artifacts.push_back(job.output_def);
        }
        if (!job.output_sdc.empty()) {
            std::error_code status;
            const auto parent = job.output_sdc.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            if (!eval("write_sdc " + tclQuote(job.output_sdc.string()), error))
                return false;
            result.artifacts.push_back(job.output_sdc);
        }
        if (write_verilog && !job.output_verilog.empty()) {
            std::error_code status;
            const auto parent = job.output_verilog.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            if (!eval("write_verilog " + tclQuote(job.output_verilog.string()), error))
                return false;
            result.artifacts.push_back(job.output_verilog);
        }
        collectMetrics(result);
        if (!job.metrics_file.empty()) {
            std::ostringstream json;
            json << "{";
            bool first = true;
            for (const auto &[name, value] : result.metrics) {
                if (!first)
                    json << ",";
                first = false;
                json << "\"" << name << "\":" << jsonNumber(value);
            }
            json << "}\n";
            if (!writeText(job.metrics_file, json.str(), error))
                return false;
            result.artifacts.push_back(job.metrics_file);
        }
        return true;
    }

    StageResult runPlacement(const StageJob &job) {
        StageResult result;
        std::string error;
        try {
            if (!loadStageInput(job, error)) {
                result.error = error;
                return result;
            }
            auto *replace = openroad->getReplace();
            gpl::PlaceOptions options;
            options.density = job.options.placement_density > 0.0 ? job.options.placement_density : 0.5;
            options.timingDrivenMode = job.options.timing_driven;
            options.routabilityDrivenMode = job.options.routability_driven;
            replace->doPlace(std::max(1, job.threads), options);
            auto *dpl = openroad->getOpendp();
            dpl->detailedPlacement(0, 0);
            openroad->getResizer()->repairDesign(0.0, 0.0, 0.0, 1.0, false, false, false);
            openroad->getEstimateParasitics()->estimateParasitics(est::ParasiticsSrc::kPlacement);
            collectTimingMetrics(result);
            if (!writeStageOutputs(job, result, error)) {
                result.error = error;
                return result;
            }
            result.success = true;
            result.tool_versions["openroad"] = openroadVersion();
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    }

    StageResult runCts(const StageJob &job) {
        StageResult result;
        std::string error;
        try {
            if (!loadStageInput(job, error)) {
                result.error = error;
                return result;
            }
            auto *cts = openroad->getTritonCts();
            cts->setRootBuffer(job.options.cts_root_buffer.c_str());
            std::string buffers;
            for (const auto &buffer : job.options.cts_buffers) {
                if (!buffers.empty())
                    buffers += " ";
                buffers += buffer;
            }
            cts->setBufferList(buffers.c_str());
            cts->runTritonCts();
            auto *sta = openroad->getSta();
            for (auto *clock : sta->cmdSdc()->clocks())
                sta->setPropagatedClock(clock, sta->cmdMode());
            openroad->getResizer()->repairClkNets(0.0);
            openroad->getOpendp()->detailedPlacement(0, 0);
            collectTimingMetrics(result);
            if (!writeStageOutputs(job, result, error)) {
                result.error = error;
                return result;
            }
            result.success = true;
            result.tool_versions["openroad"] = openroadVersion();
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    }

    StageResult runRouting(const StageJob &job) {
        StageResult result;
        std::string error;
        try {
            if (!loadStageInput(job, error)) {
                result.error = error;
                return result;
            }
            auto *db = openroad->getDb();
            auto *block = db->getChip() ? db->getChip()->getBlock() : nullptr;
            auto *tech = db->getTech();
            if (!block || !tech) {
                result.error = "No design block";
                return result;
            }
            for (const auto *name : {"zero_", "one_"}) {
                if (auto *net = block->findNet(name)) {
                    if (net->getSigType() != odb::dbSigType::SIGNAL)
                        net->setSigType(odb::dbSigType::SIGNAL);
                }
            }
            auto *router = openroad->getGlobalRouter();
            router->setNumThreads(std::max(1, job.threads));
            router->setVerbose(false);
            router->setGridOrigin(0, 0);
            router->setAllowCongestion(false);
            router->setUseCUGR(false);
            router->setCongestionIterations(50);
            const auto set_layer = [&](const std::string &name, bool minimum) {
                if (name.empty())
                    return;
                if (auto *layer = tech->findLayer(name.c_str())) {
                    if (minimum)
                        router->setMinRoutingLayer(layer->getRoutingLevel());
                    else
                        router->setMaxRoutingLayer(layer->getRoutingLevel());
                }
            };
            set_layer(job.options.routing_min_layer, true);
            set_layer(job.options.routing_max_layer, false);
            router->globalRoute(true);
            if (!job.output_def.empty()) {
                std::error_code status;
                const auto guide = job.output_def.parent_path().parent_path() / "reports" / "route.guide";
                std::filesystem::create_directories(guide.parent_path(), status);
                block->writeGuides(guide.string().c_str());
                result.artifacts.push_back(guide);
            }
            if (job.options.antenna_repair) {
                try {
                    std::error_code status;
                    const auto report = job.output_def.parent_path().parent_path() / "reports" / "antenna.rpt";
                    std::filesystem::create_directories(report.parent_path(), status);
                    auto *antenna = openroad->getAntennaChecker();
                    antenna->setReportFileName(report.string().c_str());
                    const int violations = antenna->checkAntennas(nullptr, std::max(1, job.threads), false);
                    result.metrics["antenna__violating__nets"] = static_cast<double>(violations);
                    result.metrics["antenna__violating__pins"] = static_cast<double>(antenna->pinViolationCount());
                    if (std::filesystem::is_regular_file(report, status) && !status)
                        result.artifacts.push_back(report);
                } catch (const std::exception &exception) {
                    std::cerr << "warning: antenna check skipped: " << exception.what() << "\n";
                }
            }
            auto *detailed = openroad->getTritonRoute();
            drt::ParamStruct params;
            params.num_threads = std::max(1, job.threads);
            params.verbose = 1;
            params.doPa = true;
            if (!job.output_drc.empty())
                params.outputDrcFile = job.output_drc.string();
            detailed->setParams(params);
            detailed->main();
            if (!job.options.fill_cells.empty()) {
                dpl::dbMasterSeq fillers;
                for (const auto &name : job.options.fill_cells) {
                    if (auto *master = db->findMaster(name.c_str()))
                        fillers.push_back(master);
                }
                if (!fillers.empty())
                    openroad->getOpendp()->fillerPlacement(fillers, "FILLER_", false);
            }
            openroad->getOpendp()->checkPlacement(false);
            block->globalConnect(false, false);
            result.metrics["route__drc_errors"] = static_cast<double>(detailed->getNumDRVs());
            collectRouteMetrics(result);
            if (!writeStageOutputs(job, result, error)) {
                result.error = error;
                return result;
            }
            result.success = true;
            result.tool_versions["openroad"] = openroadVersion();
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    }

    StageResult runFinal(const StageJob &job) {
        StageResult result;
        std::string error;
        try {
            if (!loadStageInput(job, error)) {
                result.error = error;
                return result;
            }
            auto *ext = openroad->getOpenRCX();
            if (!job.options.extraction_model.empty()) {
                ext->define_process_corner(0, "X");
                ExtractOptions extract_options;
                extract_options.ext_model_file = job.options.extraction_model.c_str();
                ext->extract(extract_options);
                if (!job.output_spef.empty()) {
                    std::error_code status;
                    const auto parent = job.output_spef.parent_path();
                    if (!parent.empty())
                        std::filesystem::create_directories(parent, status);
                    SpefOptions spef_options;
                    spef_options.file = job.output_spef.c_str();
                    ext->write_spef(spef_options);
                    result.artifacts.push_back(job.output_spef);
                    if (!eval("read_spef " + tclQuote(job.output_spef.string()), error))
                        std::cerr << "warning: unable to read back the extracted SPEF: " << error << "\n";
                }
            }
            collectPowerMetrics(result);
            collectTimingMetrics(result);
            if (!writeStageOutputs(job, result, error, true)) {
                result.error = error;
                return result;
            }
            result.success = true;
            result.tool_versions["openroad"] = openroadVersion();
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    }

    StageResult runFloorplan(const StageJob &job, const std::function<void(const std::string &)> &log) {
        StageResult result;
        std::string error;
        auto *db = openroad->getDb();
        auto *logger = openroad->getLogger();

        if (!job.input_db.empty()) {
            if (!std::filesystem::is_regular_file(job.input_db)) {
                result.error = "Input ODB does not exist: " + job.input_db.string();
                return result;
            }
            openroad->readDb(job.input_db.string().c_str());
        } else {
            bool first_tech = true;
            for (const auto &lef : job.tech_lef) {
                if (!std::filesystem::is_regular_file(lef)) {
                    result.error = "Technology LEF does not exist: " + lef.string();
                    return result;
                }
                const auto stem = lef.stem().string();
                if (first_tech)
                    openroad->readLef(lef.string().c_str(), stem.c_str(), stem.c_str(), true, true);
                else
                    openroad->readLef(lef.string().c_str(), stem.c_str(), "", false, true);
                first_tech = false;
            }
            for (const auto &lef : job.cell_lef) {
                if (!std::filesystem::is_regular_file(lef)) {
                    result.error = "Cell LEF does not exist: " + lef.string();
                    return result;
                }
                const auto stem = lef.stem().string();
                openroad->readLef(lef.string().c_str(), stem.c_str(), "", false, true);
            }
            auto *sta = openroad->getSta();
            for (const auto &liberty : job.liberty) {
                if (!std::filesystem::is_regular_file(liberty)) {
                    result.error = "Liberty file does not exist: " + liberty.string();
                    return result;
                }
                sta->readLiberty(liberty.string(), sta->cmdScene(), sta::MinMaxAll::all(), false);
            }
            if (!std::filesystem::is_regular_file(job.verilog)) {
                result.error = "Verilog netlist does not exist: " + job.verilog.string();
                return result;
            }
            openroad->readVerilog(job.verilog.string().c_str());
            openroad->linkDesign(job.top.c_str(), true);
        }

        if (!job.sdc.empty()) {
            if (!std::filesystem::is_regular_file(job.sdc)) {
                result.error = "SDC file does not exist: " + job.sdc.string();
                return result;
            }
            if (!eval("read_sdc " + tclQuote(job.sdc.string()), error)) {
                result.error = "read_sdc failed: " + error;
                return result;
            }
        }

        auto *block = db->getChip() ? db->getChip()->getBlock() : nullptr;
        if (!block) {
            result.error = "No design block after reading the netlist";
            return result;
        }

        ifp::InitFloorplan floorplan(block, logger, openroad->getDbNetwork());
        const auto site_name = job.options.site.empty() ? "" : job.options.site;
        auto *site = site_name.empty() ? nullptr : floorplan.findSite(site_name.c_str());
        if (!site_name.empty() && !site) {
            result.error = "Placement site not found: " + site_name;
            return result;
        }
        if (!site) {
            result.error = "No placement site was provided";
            return result;
        }

        const int dbu = block->getDbUnitsPerMicron();
        const int core_space = static_cast<int>(std::llround(job.options.core_space * dbu));
        if (job.options.die_area) {
            const auto die = toRect(*job.options.die_area, dbu);
            odb::Rect core;
            if (job.options.core_area)
                core = toRect(*job.options.core_area, dbu);
            else
                core = odb::Rect(die.xMin() + core_space, die.yMin() + core_space, die.xMax() - core_space,
                                 die.yMax() - core_space);
            floorplan.initFloorplan(die, core, site);
        } else {
            floorplan.initFloorplan(job.options.utilization, job.options.aspect_ratio, core_space, core_space,
                                    core_space, core_space, site);
        }
        floorplan.makeTracks();

        auto *tech = db->getTech();
        auto *io = openroad->getIOPlacer();
        const auto horizontal = job.options.io_horizontal_layers;
        const auto vertical = job.options.io_vertical_layers;
        if (!block->getBTerms().empty() && !horizontal.empty() && !vertical.empty()) {
            io->clear();
            for (const auto &name : horizontal) {
                if (auto *layer = tech->findLayer(name.c_str()))
                    io->addHorLayer(layer);
            }
            for (const auto &name : vertical) {
                if (auto *layer = tech->findLayer(name.c_str()))
                    io->addVerLayer(layer);
            }
            io->runHungarianMatching(false);
        }

        if (!source(job.tapcell_script, error)) {
            result.error = error;
            return result;
        }
        if (!source(job.pdn_script, error)) {
            result.error = error;
            return result;
        }

        if (!job.output_db.empty()) {
            std::error_code status;
            const auto parent = job.output_db.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            openroad->writeDb(job.output_db.string().c_str());
            result.artifacts.push_back(job.output_db);
        }
        if (!job.output_def.empty()) {
            std::error_code status;
            const auto parent = job.output_def.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            openroad->writeDef(job.output_def.string().c_str(), "5.8");
            result.artifacts.push_back(job.output_def);
        }
        if (!job.output_sdc.empty()) {
            std::error_code status;
            const auto parent = job.output_sdc.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, status);
            if (!eval("write_sdc " + tclQuote(job.output_sdc.string()), error)) {
                result.error = "write_sdc failed: " + error;
                return result;
            }
            result.artifacts.push_back(job.output_sdc);
        }

        const auto core_area = block->getCoreArea();
        const auto die_rect = block->getDieArea();
        const double dbu_area = static_cast<double>(dbu) * static_cast<double>(dbu);
        result.metrics["design__core__area"] = static_cast<double>(core_area.area()) / dbu_area;
        result.metrics["design__die__area"] = static_cast<double>(die_rect.area()) / dbu_area;
        result.metrics["design__instance__count"] =
            static_cast<double>(std::distance(block->getInsts().begin(), block->getInsts().end()));
        result.metrics["design__bterm__count"] =
            static_cast<double>(std::distance(block->getBTerms().begin(), block->getBTerms().end()));

        if (!job.metrics_file.empty()) {
            std::ostringstream json;
            json << "{";
            bool first = true;
            for (const auto &[name, value] : result.metrics) {
                if (!first)
                    json << ",";
                first = false;
                json << "\"" << name << "\":" << jsonNumber(value);
            }
            json << "}\n";
            if (!writeText(job.metrics_file, json.str(), error)) {
                result.error = error;
                return result;
            }
            result.artifacts.push_back(job.metrics_file);
        }

        result.success = true;
        result.tool_versions["openroad"] = openroadVersion();
        (void)log;
        return result;
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { shutdown(); }

bool Engine::initialize(const EngineOptions &options, std::string &error) {
    if (impl_->openroad)
        return true;

    impl_->log_file = options.log_file;
    impl_->metrics_file = options.metrics_file;
    impl_->threads = std::max(1, options.threads);

    if (!options.log_file.empty()) {
        std::error_code status;
        const auto parent = options.log_file.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, status);
    }

    impl_->interp = Tcl_CreateInterp();
    if (!impl_->interp) {
        error = "Unable to create the Tcl interpreter";
        return false;
    }
    if (in_bazel::SetupTclEnvironment(impl_->interp) != TCL_OK) {
        error = "Unable to mount the embedded Tcl library";
        return false;
    }
    if (Tcl_Init(impl_->interp) != TCL_OK) {
        error = std::string("Tcl initialization failed: ") + Tcl_GetStringResult(impl_->interp);
        return false;
    }

    try {
        impl_->tech = std::make_unique<ord::Tech>(impl_->interp);
        impl_->design = std::make_unique<ord::Design>(impl_->tech.get());
        Tcl_SetAssocData(impl_->interp, "design", nullptr, impl_->design.get());
        // The stage metrics file is written by the engine after the stage.
        // Do not hand it to OpenROAD's exit handler, which would overwrite it
        // with the Tcl command metrics.
        ord::initOpenRoad(impl_->interp, options.log_file.empty() ? nullptr : options.log_file.string().c_str(),
                          nullptr, true);
        impl_->openroad = ord::OpenRoad::openRoad();
        if (!impl_->openroad) {
            error = "OpenROAD initialization failed";
            return false;
        }
        impl_->openroad->setThreadCount(impl_->threads, false);
    } catch (const std::exception &exception) {
        error = std::string("OpenROAD initialization failed: ") + exception.what();
        return false;
    }
    return true;
}

void Engine::shutdown() {
    impl_->openroad = nullptr;
    impl_->design.reset();
    impl_->tech.reset();
    impl_->interp = nullptr;
}

StageResult Engine::runStage(const StageJob &job, const std::function<void(const std::string &)> &log) {
    if (!impl_->openroad) {
        StageResult result;
        result.error = "OpenROAD engine is not initialized";
        return result;
    }
    try {
        switch (job.stage) {
        case Stage::floorplan:
            return impl_->runFloorplan(job, log);
        case Stage::placement:
            return impl_->runPlacement(job);
        case Stage::cts:
            return impl_->runCts(job);
        case Stage::routing:
            return impl_->runRouting(job);
        case Stage::final:
            return impl_->runFinal(job);
        }
    } catch (const std::exception &exception) {
        StageResult result;
        result.error = exception.what();
        return result;
    }
    StageResult result;
    result.error = "Unsupported implementation stage";
    return result;
}

HeatMapResult Engine::exportHeatMap(const HeatMapJob &job) {
    HeatMapResult result;
    if (!impl_->openroad) {
        result.error = "OpenROAD engine is not initialized";
        return result;
    }
    if (job.input_db.empty() || !std::filesystem::is_regular_file(job.input_db)) {
        result.error = "Input ODB does not exist: " + job.input_db.string();
        return result;
    }
    if (job.output_csv.empty()) {
        result.error = "No heat-map output path was provided";
        return result;
    }
    try {
        impl_->openroad->readDb(job.input_db.string().c_str());
        auto *sta = impl_->openroad->getSta();
        for (const auto &liberty : job.liberty) {
            if (!std::filesystem::is_regular_file(liberty)) {
                result.error = "Liberty file does not exist: " + liberty.string();
                return result;
            }
            sta->readLiberty(liberty.string(), sta->cmdScene(), sta::MinMaxAll::all(), false);
        }
        if (!job.sdc.empty()) {
            if (!std::filesystem::is_regular_file(job.sdc)) {
                result.error = "SDC file does not exist: " + job.sdc.string();
                return result;
            }
            std::string error;
            if (!impl_->eval("read_sdc " + tclQuote(job.sdc.string()), error)) {
                result.error = "read_sdc failed: " + error;
                return result;
            }
        }

        std::string name;
        switch (job.kind) {
        case HeatMapKind::placement_density:
            name = "Placement";
            break;
        case HeatMapKind::routing_congestion:
            name = "Routing";
            break;
        case HeatMapKind::power_density:
            name = "Power";
            break;
        case HeatMapKind::pin_density:
            name = "Pin";
            break;
        case HeatMapKind::rudy:
            name = "RUDY";
            break;
        case HeatMapKind::ir_drop:
            name = "IRDrop";
            break;
        }

        auto *gui = web::Gui::get();
        if (!gui || !gui->getHeatMap(name)) {
            result.error = "OpenROAD heat map is not available: " + name;
            return result;
        }
        std::error_code status;
        const auto parent = job.output_csv.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, status);
        gui->dumpHeatMap(name, job.output_csv.string());
        if (!std::filesystem::is_regular_file(job.output_csv, status) || status) {
            result.error = "OpenROAD did not write the heat map: " + job.output_csv.string();
            return result;
        }
        result.csv = job.output_csv;
        result.success = true;
    } catch (const std::exception &exception) {
        result.error = exception.what();
    }
    return result;
}

std::string Engine::version() { return "0.1.0"; }

std::string Engine::openroadVersion() {
    const auto *version = ord::OpenRoad::getVersion();
    const auto *git = ord::OpenRoad::getGitDescribe();
    return std::string(version ? version : "unknown") + " " + (git ? git : "unknown");
}

} // namespace ice::openroad
