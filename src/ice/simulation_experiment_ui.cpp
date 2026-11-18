#include "simulation_experiment_ui.hpp"

#include "icelib/simulation.hpp"
#include "icelib/simulation_store.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

namespace {

QDoubleSpinBox *makeDoubleSpin(double minimum, double maximum, int decimals, double value) {
    auto *spin = new QDoubleSpinBox;
    spin->setRange(minimum, maximum);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setKeyboardTracking(false);
    return spin;
}

QTableWidget *makeTable(const QStringList &headers) {
    auto *table = new QTableWidget;
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    return table;
}

QString numberText(double value) {
    std::ostringstream stream;
    stream.precision(12);
    stream << value;
    return QString::fromStdString(stream.str());
}

std::map<std::string, double> parseNumberMap(const QString &text, bool *ok) {
    std::map<std::string, double> values;
    if (ok)
        *ok = true;
    const auto parts = text.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        const auto trimmed = part.trimmed();
        if (trimmed.isEmpty())
            continue;
        const auto separator = trimmed.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            if (ok)
                *ok = false;
            return {};
        }
        bool numberOk = false;
        const auto value = trimmed.mid(separator + 1).trimmed().toDouble(&numberOk);
        if (!numberOk) {
            if (ok)
                *ok = false;
            return {};
        }
        values[trimmed.left(separator).trimmed().toStdString()] = value;
    }
    return values;
}

QString numberMapText(const std::map<std::string, double> &values) {
    QStringList parts;
    for (const auto &[name, value] : values)
        parts << QStringLiteral("%1=%2").arg(QString::fromStdString(name), numberText(value));
    return parts.join(QStringLiteral(", "));
}

QTableWidgetItem *item(const QString &text) { return new QTableWidgetItem(text); }

} // namespace

SimulationExperimentDialog::SimulationExperimentDialog(const ice::Project &project,
                                                       const ice::SimulationItem &simulation,
                                                       const std::filesystem::path &projectRoot, QWidget *parent)
    : QDialog(parent), project_(project), projectRoot_(projectRoot), item_(simulation) {
    setWindowTitle(QStringLiteral("Simulation Experiment — %1").arg(QString::fromStdString(item_.name)));
    resize(980, 720);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    // Analysis parameters.
    auto *analysisPage = new QWidget;
    auto *analysisLayout = new QFormLayout(analysisPage);
    startTime_ = makeDoubleSpin(0.0, 1e12, 15, item_.start);
    temperature_ = makeDoubleSpin(-273.15, 1000.0, 3, item_.temperature);
    modelSection_ = new QLineEdit(QString::fromStdString(item_.modelSection));
    modelSection_->setPlaceholderText(QStringLiteral("Optional .lib section override"));
    dcSource_ = new QLineEdit(QString::fromStdString(item_.dcSource));
    dcSource_->setPlaceholderText(QStringLiteral("Auto-detect first voltage/current source"));
    dcStart_ = makeDoubleSpin(-1e12, 1e12, 12, item_.dcStart);
    dcStop_ = makeDoubleSpin(-1e12, 1e12, 12, item_.dcStop);
    dcStep_ = makeDoubleSpin(1e-15, 1e12, 15, item_.dcStep);
    acVariation_ = new QComboBox;
    acVariation_->addItems({QStringLiteral("dec"), QStringLiteral("oct"), QStringLiteral("lin")});
    acVariation_->setCurrentText(QString::fromStdString(item_.acVariation));
    acPoints_ = new QSpinBox;
    acPoints_->setRange(1, 100000);
    acPoints_->setValue(item_.acPoints);
    acStart_ = makeDoubleSpin(1e-15, 1e15, 9, item_.acStart);
    acStop_ = makeDoubleSpin(1e-15, 1e15, 9, item_.acStop);
    noiseOutput_ = new QLineEdit(QString::fromStdString(item_.noiseOutput));
    noiseInput_ = new QLineEdit(QString::fromStdString(item_.noiseInput));
    noiseVariation_ = new QComboBox;
    noiseVariation_->addItems({QStringLiteral("dec"), QStringLiteral("oct"), QStringLiteral("lin")});
    noiseVariation_->setCurrentText(QString::fromStdString(item_.noiseVariation));
    noisePoints_ = new QSpinBox;
    noisePoints_->setRange(1, 100000);
    noisePoints_->setValue(item_.noisePoints);
    noiseStart_ = makeDoubleSpin(1e-15, 1e15, 9, item_.noiseStart);
    noiseStop_ = makeDoubleSpin(1e-15, 1e15, 9, item_.noiseStop);
    tfOutput_ = new QLineEdit(QString::fromStdString(item_.tfOutput));
    tfSource_ = new QLineEdit(QString::fromStdString(item_.tfSource));

    analysisLayout->addRow(QStringLiteral("Transient start (s)"), startTime_);
    analysisLayout->addRow(QStringLiteral("Temperature (°C)"), temperature_);
    analysisLayout->addRow(QStringLiteral("Model section"), modelSection_);
    analysisLayout->addRow(QStringLiteral("DC source"), dcSource_);
    analysisLayout->addRow(QStringLiteral("DC start"), dcStart_);
    analysisLayout->addRow(QStringLiteral("DC stop"), dcStop_);
    analysisLayout->addRow(QStringLiteral("DC step"), dcStep_);
    analysisLayout->addRow(QStringLiteral("AC variation"), acVariation_);
    analysisLayout->addRow(QStringLiteral("AC points"), acPoints_);
    analysisLayout->addRow(QStringLiteral("AC start (Hz)"), acStart_);
    analysisLayout->addRow(QStringLiteral("AC stop (Hz)"), acStop_);
    analysisLayout->addRow(QStringLiteral("Noise output"), noiseOutput_);
    analysisLayout->addRow(QStringLiteral("Noise input source"), noiseInput_);
    analysisLayout->addRow(QStringLiteral("Noise variation"), noiseVariation_);
    analysisLayout->addRow(QStringLiteral("Noise points"), noisePoints_);
    analysisLayout->addRow(QStringLiteral("Noise start (Hz)"), noiseStart_);
    analysisLayout->addRow(QStringLiteral("Noise stop (Hz)"), noiseStop_);
    analysisLayout->addRow(QStringLiteral("TF output"), tfOutput_);
    analysisLayout->addRow(QStringLiteral("TF input source"), tfSource_);
    tabs->addTab(analysisPage, QStringLiteral("Analysis"));

    // Corners.
    auto *cornersPage = new QWidget;
    auto *cornersLayout = new QVBoxLayout(cornersPage);
    corners_ = makeTable({QStringLiteral("Name"), QStringLiteral("Model section"), QStringLiteral("Temperature"),
                          QStringLiteral("Supplies"), QStringLiteral("Parameters")});
    auto *cornerButtons = new QHBoxLayout;
    auto *addCorner = new QPushButton(QStringLiteral("Add"));
    auto *defaultCorners = new QPushButton(QStringLiteral("Load PDK TT/FF/SS"));
    auto *removeCorner = new QPushButton(QStringLiteral("Remove"));
    cornerButtons->addWidget(addCorner);
    cornerButtons->addWidget(defaultCorners);
    cornerButtons->addStretch();
    cornerButtons->addWidget(removeCorner);
    cornersLayout->addWidget(corners_);
    cornersLayout->addLayout(cornerButtons);
    connect(addCorner, &QPushButton::clicked, this, [this] { addCornerRow(); });
    connect(defaultCorners, &QPushButton::clicked, this, [this] { loadPdkCorners(); });
    connect(removeCorner, &QPushButton::clicked, this, [this] { removeSelectedRow(corners_); });
    for (const auto &corner : item_.corners)
        addCornerRow(corner);
    tabs->addTab(cornersPage, QStringLiteral("Corners"));

    // Sweeps.
    auto *sweepsPage = new QWidget;
    auto *sweepsLayout = new QVBoxLayout(sweepsPage);
    sweeps_ = makeTable({QStringLiteral("Parameter"), QStringLiteral("Values (comma/space separated)")});
    auto *sweepButtons = new QHBoxLayout;
    auto *addSweep = new QPushButton(QStringLiteral("Add"));
    auto *removeSweep = new QPushButton(QStringLiteral("Remove"));
    sweepButtons->addWidget(addSweep);
    sweepButtons->addStretch();
    sweepButtons->addWidget(removeSweep);
    sweepsLayout->addWidget(sweeps_);
    sweepsLayout->addLayout(sweepButtons);
    connect(addSweep, &QPushButton::clicked, this, [this] { addSweepRow(); });
    connect(removeSweep, &QPushButton::clicked, this, [this] { removeSelectedRow(sweeps_); });
    for (const auto &sweep : item_.sweeps)
        addSweepRow(sweep);
    tabs->addTab(sweepsPage, QStringLiteral("Sweeps"));

    // Monte Carlo.
    auto *mcPage = new QWidget;
    auto *mcLayout = new QFormLayout(mcPage);
    monteCarloEnabled_ = new QCheckBox(QStringLiteral("Enable Monte Carlo"));
    monteCarloEnabled_->setChecked(item_.monteCarlo.enabled);
    monteCarloSamples_ = new QSpinBox;
    monteCarloSamples_->setRange(1, 100000);
    monteCarloSamples_->setValue(item_.monteCarlo.samples);
    temperatureSigma_ = makeDoubleSpin(0.0, 1000.0, 4, item_.monteCarlo.temperatureSigma);
    supplySigma_ = makeDoubleSpin(0.0, 1000.0, 4, item_.monteCarlo.supplySigmaPercent);
    supplySource_ = new QLineEdit(QString::fromStdString(item_.monteCarlo.supplySource));
    supplyNominal_ = makeDoubleSpin(-1e9, 1e9, 9, item_.monteCarlo.supplyNominal);
    monteCarloSeed_ = new QSpinBox;
    monteCarloSeed_->setRange(0, std::numeric_limits<int>::max());
    monteCarloSeed_->setValue(static_cast<int>(item_.monteCarlo.seed));
    parameterSigma_ = makeTable({QStringLiteral("Parameter"), QStringLiteral("Sigma (%)")});
    auto *sigmaButtons = new QHBoxLayout;
    auto *addSigma = new QPushButton(QStringLiteral("Add"));
    auto *removeSigma = new QPushButton(QStringLiteral("Remove"));
    sigmaButtons->addWidget(addSigma);
    sigmaButtons->addStretch();
    sigmaButtons->addWidget(removeSigma);
    auto *sigmaWidget = new QWidget;
    auto *sigmaLayout = new QVBoxLayout(sigmaWidget);
    sigmaLayout->setContentsMargins(0, 0, 0, 0);
    sigmaLayout->addWidget(parameterSigma_);
    sigmaLayout->addLayout(sigmaButtons);
    connect(addSigma, &QPushButton::clicked, this, [this] {
        const auto row = parameterSigma_->rowCount();
        parameterSigma_->insertRow(row);
        parameterSigma_->setItem(row, 0, item({}));
        parameterSigma_->setItem(row, 1, item(QStringLiteral("0")));
    });
    connect(removeSigma, &QPushButton::clicked, this, [this] { removeSelectedRow(parameterSigma_); });
    for (const auto &[name, sigma] : item_.monteCarlo.parameterSigmaPercent) {
        const auto row = parameterSigma_->rowCount();
        parameterSigma_->insertRow(row);
        parameterSigma_->setItem(row, 0, item(QString::fromStdString(name)));
        parameterSigma_->setItem(row, 1, item(numberText(sigma)));
    }
    mcLayout->addRow(monteCarloEnabled_);
    mcLayout->addRow(QStringLiteral("Samples"), monteCarloSamples_);
    mcLayout->addRow(QStringLiteral("Temperature sigma (°C)"), temperatureSigma_);
    mcLayout->addRow(QStringLiteral("Supply sigma (%)"), supplySigma_);
    mcLayout->addRow(QStringLiteral("Supply source"), supplySource_);
    mcLayout->addRow(QStringLiteral("Supply nominal (V)"), supplyNominal_);
    mcLayout->addRow(QStringLiteral("Seed"), monteCarloSeed_);
    mcLayout->addRow(QStringLiteral("Parameter sigma"), sigmaWidget);
    tabs->addTab(mcPage, QStringLiteral("Monte Carlo"));

    // Declarative measurements.
    auto *measurePage = new QWidget;
    auto *measureLayout = new QVBoxLayout(measurePage);
    measurements_ =
        makeTable({QStringLiteral("Name"), QStringLiteral("Metric"), QStringLiteral("Signal"), QStringLiteral("Input"),
                   QStringLiteral("Voltage"), QStringLiteral("Threshold"), QStringLiteral("Tolerance")});
    auto *measureButtons = new QHBoxLayout;
    auto *addMeasure = new QPushButton(QStringLiteral("Add"));
    auto *removeMeasure = new QPushButton(QStringLiteral("Remove"));
    measureButtons->addWidget(addMeasure);
    measureButtons->addStretch();
    measureButtons->addWidget(removeMeasure);
    measureLayout->addWidget(measurements_);
    measureLayout->addLayout(measureButtons);
    connect(addMeasure, &QPushButton::clicked, this, [this] { addMeasurementRow(); });
    connect(removeMeasure, &QPushButton::clicked, this, [this] { removeSelectedRow(measurements_); });
    for (const auto &spec : item_.measurements)
        addMeasurementRow(spec);
    tabs->addTab(measurePage, QStringLiteral("Measurements"));

    // Results.
    auto *resultsPage = new QWidget;
    auto *resultsLayout = new QVBoxLayout(resultsPage);
    results_ = makeTable(
        {QStringLiteral("Variant"), QStringLiteral("State"), QStringLiteral("Measurements"), QStringLiteral("Stored")});
    resultsLayout->addWidget(results_);
    tabs->addTab(resultsPage, QStringLiteral("Results"));

    layout->addWidget(tabs, 1);
    auto *buttons = new QDialogButtonBox;
    runButton_ = new QPushButton(QStringLiteral("Run Experiment"));
    runButton_->setDefault(false);
    buttons->addButton(runButton_, QDialogButtonBox::ActionRole);
    buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(runButton_, &QPushButton::clicked, this, [this] { runExperiment(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SimulationExperimentDialog::accept() {
    if (!applyItem())
        return;
    QDialog::accept();
}

void SimulationExperimentDialog::addCornerRow(const ice::SimulationCorner &corner) {
    const auto row = corners_->rowCount();
    corners_->insertRow(row);
    corners_->setItem(row, 0, item(QString::fromStdString(corner.name)));
    corners_->setItem(row, 1, item(QString::fromStdString(corner.modelSection)));
    corners_->setItem(row, 2, item(numberText(corner.temperature)));
    corners_->setItem(row, 3, item(numberMapText(corner.sourceOverrides)));
    corners_->setItem(row, 4, item(numberMapText(corner.parameters)));
}

void SimulationExperimentDialog::loadPdkCorners() {
    const auto schematic = project_.schematicCells.find(item_.schematic);
    if (schematic == project_.schematicCells.end()) {
        QMessageBox::information(this, QStringLiteral("Load PDK Corners"),
                                 QStringLiteral("Select a simulation with a schematic first."));
        return;
    }
    const auto corners = ice::pdkSimulationCorners(schematic->second.pdk);
    if (corners.empty()) {
        QMessageBox::information(this, QStringLiteral("Load PDK Corners"),
                                 QStringLiteral("The active PDK does not declare TT/FF/SS model sections."));
        return;
    }
    corners_->setRowCount(0);
    for (const auto &corner : corners)
        addCornerRow(corner);
}

void SimulationExperimentDialog::addSweepRow(const ice::SimulationSweep &sweep) {
    const auto row = sweeps_->rowCount();
    sweeps_->insertRow(row);
    sweeps_->setItem(row, 0, item(QString::fromStdString(sweep.parameter)));
    QStringList values;
    for (const auto value : sweep.values)
        values << numberText(value);
    sweeps_->setItem(row, 1, item(values.join(QStringLiteral(", "))));
}

void SimulationExperimentDialog::addMeasurementRow(const ice::SimulationMeasurementSpec &spec) {
    const auto row = measurements_->rowCount();
    measurements_->insertRow(row);
    measurements_->setItem(row, 0, item(QString::fromStdString(spec.name)));
    measurements_->setItem(row, 1, item(QString::fromStdString(spec.metric)));
    measurements_->setItem(row, 2, item(QString::fromStdString(spec.signal)));
    measurements_->setItem(row, 3, item(QString::fromStdString(spec.input)));
    measurements_->setItem(row, 4, item(spec.voltage == 0.0 ? QString() : numberText(spec.voltage)));
    measurements_->setItem(row, 5, item(spec.threshold ? numberText(*spec.threshold) : QString()));
    measurements_->setItem(row, 6, item(numberText(spec.tolerance)));
}

void SimulationExperimentDialog::removeSelectedRow(QTableWidget *table) {
    if (!table)
        return;
    const auto row = table->currentRow();
    if (row >= 0)
        table->removeRow(row);
}

bool SimulationExperimentDialog::applyItem() {
    item_.start = startTime_->value();
    item_.temperature = temperature_->value();
    item_.modelSection = modelSection_->text().trimmed().toStdString();
    item_.dcSource = dcSource_->text().trimmed().toStdString();
    item_.dcStart = dcStart_->value();
    item_.dcStop = dcStop_->value();
    item_.dcStep = dcStep_->value();
    item_.acVariation = acVariation_->currentText().toStdString();
    item_.acPoints = acPoints_->value();
    item_.acStart = acStart_->value();
    item_.acStop = acStop_->value();
    item_.noiseOutput = noiseOutput_->text().trimmed().toStdString();
    item_.noiseInput = noiseInput_->text().trimmed().toStdString();
    item_.noiseVariation = noiseVariation_->currentText().toStdString();
    item_.noisePoints = noisePoints_->value();
    item_.noiseStart = noiseStart_->value();
    item_.noiseStop = noiseStop_->value();
    item_.tfOutput = tfOutput_->text().trimmed().toStdString();
    item_.tfSource = tfSource_->text().trimmed().toStdString();
    if (item_.dcStep <= 0.0 || item_.acStop < item_.acStart || item_.acPoints <= 0) {
        QMessageBox::warning(this, QStringLiteral("Simulation Settings"),
                             QStringLiteral("DC step, AC points, and AC stop frequency are invalid."));
        return false;
    }

    item_.corners.clear();
    for (int row = 0; row < corners_->rowCount(); ++row) {
        const auto name = corners_->item(row, 0) ? corners_->item(row, 0)->text().trimmed() : QString();
        if (name.isEmpty())
            continue;
        bool ok = false;
        ice::SimulationCorner corner;
        corner.name = name.toStdString();
        corner.modelSection =
            corners_->item(row, 1) ? corners_->item(row, 1)->text().trimmed().toStdString() : std::string{};
        corner.temperature = corners_->item(row, 2) ? corners_->item(row, 2)->text().toDouble(&ok) : 27.0;
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Simulation Corner"),
                                 QStringLiteral("Corner '%1' has an invalid temperature.").arg(name));
            return false;
        }
        corner.sourceOverrides =
            parseNumberMap(corners_->item(row, 3) ? corners_->item(row, 3)->text() : QString(), &ok);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Simulation Corner"),
                                 QStringLiteral("Corner '%1' has invalid supplies (use name=value).").arg(name));
            return false;
        }
        corner.parameters = parseNumberMap(corners_->item(row, 4) ? corners_->item(row, 4)->text() : QString(), &ok);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Simulation Corner"),
                                 QStringLiteral("Corner '%1' has invalid parameters.").arg(name));
            return false;
        }
        item_.corners.push_back(std::move(corner));
    }

    item_.sweeps.clear();
    for (int row = 0; row < sweeps_->rowCount(); ++row) {
        const auto parameter = sweeps_->item(row, 0) ? sweeps_->item(row, 0)->text().trimmed() : QString();
        if (parameter.isEmpty())
            continue;
        ice::SimulationSweep sweep;
        sweep.parameter = parameter.toStdString();
        const auto text = sweeps_->item(row, 1) ? sweeps_->item(row, 1)->text() : QString();
        for (const auto &valueText : text.split(QRegularExpression(QStringLiteral("[, \\t]+")), Qt::SkipEmptyParts)) {
            bool ok = false;
            const auto value = valueText.toDouble(&ok);
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("Simulation Sweep"),
                                     QStringLiteral("Sweep '%1' has an invalid value: %2").arg(parameter, valueText));
                return false;
            }
            sweep.values.push_back(value);
        }
        if (sweep.values.empty()) {
            QMessageBox::warning(this, QStringLiteral("Simulation Sweep"),
                                 QStringLiteral("Sweep '%1' needs at least one value.").arg(parameter));
            return false;
        }
        item_.sweeps.push_back(std::move(sweep));
    }

    item_.monteCarlo.enabled = monteCarloEnabled_->isChecked();
    item_.monteCarlo.samples = monteCarloSamples_->value();
    item_.monteCarlo.temperatureSigma = temperatureSigma_->value();
    item_.monteCarlo.supplySigmaPercent = supplySigma_->value();
    item_.monteCarlo.supplySource = supplySource_->text().trimmed().toStdString();
    item_.monteCarlo.supplyNominal = supplyNominal_->value();
    item_.monteCarlo.seed = static_cast<std::uint32_t>(monteCarloSeed_->value());
    item_.monteCarlo.parameterSigmaPercent.clear();
    for (int row = 0; row < parameterSigma_->rowCount(); ++row) {
        const auto name = parameterSigma_->item(row, 0) ? parameterSigma_->item(row, 0)->text().trimmed() : QString();
        if (name.isEmpty())
            continue;
        bool ok = false;
        const auto sigma = parameterSigma_->item(row, 1) ? parameterSigma_->item(row, 1)->text().toDouble(&ok) : 0.0;
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Monte Carlo"),
                                 QStringLiteral("Parameter sigma for '%1' is invalid.").arg(name));
            return false;
        }
        item_.monteCarlo.parameterSigmaPercent[name.toStdString()] = sigma;
    }

    item_.measurements.clear();
    for (int row = 0; row < measurements_->rowCount(); ++row) {
        const auto name = measurements_->item(row, 0) ? measurements_->item(row, 0)->text().trimmed() : QString();
        const auto metric = measurements_->item(row, 1) ? measurements_->item(row, 1)->text().trimmed() : QString();
        if (name.isEmpty() && metric.isEmpty())
            continue;
        if (name.isEmpty() || metric.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Simulation Measurements"),
                                 QStringLiteral("Each measurement needs a name and a metric."));
            return false;
        }
        ice::SimulationMeasurementSpec spec;
        spec.name = name.toStdString();
        spec.metric = metric.toStdString();
        spec.signal =
            measurements_->item(row, 2) ? measurements_->item(row, 2)->text().trimmed().toStdString() : std::string{};
        spec.input =
            measurements_->item(row, 3) ? measurements_->item(row, 3)->text().trimmed().toStdString() : std::string{};
        auto parseOptional = [&](int column, double &target) {
            const auto text =
                measurements_->item(row, column) ? measurements_->item(row, column)->text().trimmed() : QString();
            if (text.isEmpty())
                return true;
            bool ok = false;
            target = text.toDouble(&ok);
            return ok;
        };
        double voltage = 0.0;
        double threshold = 0.0;
        double tolerance = 0.02;
        if (!parseOptional(4, voltage) || !parseOptional(5, threshold) || !parseOptional(6, tolerance)) {
            QMessageBox::warning(this, QStringLiteral("Simulation Measurements"),
                                 QStringLiteral("Measurement '%1' has an invalid numeric field.").arg(name));
            return false;
        }
        spec.voltage = voltage;
        if (measurements_->item(row, 5) && !measurements_->item(row, 5)->text().trimmed().isEmpty())
            spec.threshold = threshold;
        spec.tolerance = tolerance;
        item_.measurements.push_back(std::move(spec));
    }
    if (item_.monteCarlo.enabled && item_.monteCarlo.samples <= 0) {
        QMessageBox::warning(this, QStringLiteral("Monte Carlo"),
                             QStringLiteral("Monte Carlo requires at least one sample."));
        return false;
    }
    return true;
}

void SimulationExperimentDialog::runExperiment() {
    if (!applyItem())
        return;
    runButton_->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();
    std::vector<ice::ExperimentPoint> points;
    try {
        points = ice::runSimulationExperiment(project_, item_, std::chrono::seconds(60), {}, projectRoot_);
    } catch (const std::exception &error) {
        QApplication::restoreOverrideCursor();
        runButton_->setEnabled(true);
        QMessageBox::critical(this, QStringLiteral("Run Experiment"), QString::fromStdString(error.what()));
        return;
    }
    std::vector<std::filesystem::path> stored;
    if (!projectRoot_.empty()) {
        try {
            const auto request = ice::simulationRequestForItem(project_, item_, projectRoot_);
            for (const auto &point : points) {
                ice::SimulationRunContext context;
                context.projectRoot = projectRoot_;
                context.projectIdentity = project_.identity;
                context.projectName = project_.name;
                context.simulationId = item_.id;
                context.simulationName = item_.name;
                context.variant = point.variant;
                context.schematicId = item_.schematic;
                if (const auto schematic = project_.schematicCells.find(item_.schematic);
                    schematic != project_.schematicCells.end())
                    context.schematicName = schematic->second.name;
                context.request = request;
                context.directives = item_.directives;
                context.graphs = item_.graphs;
                stored.push_back(ice::writeSimulationRun(context, point.result).path);
            }
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("Run Experiment"),
                                 QStringLiteral("Results ran but could not all be stored: %1")
                                     .arg(QString::fromStdString(error.what())));
        }
    }
    QApplication::restoreOverrideCursor();
    runButton_->setEnabled(true);
    showResults(points, stored);
}

void SimulationExperimentDialog::showResults(const std::vector<ice::ExperimentPoint> &points,
                                             const std::vector<std::filesystem::path> &storedPaths) {
    results_->setRowCount(0);
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto &point = points[index];
        const auto row = results_->rowCount();
        results_->insertRow(row);
        results_->setItem(row, 0, item(QString::fromStdString(point.variant)));
        results_->setItem(row, 1, item(QString::fromStdString(ice::jobStateName(point.result.state))));
        QStringList measurements;
        for (const auto &measurement : point.result.measurements) {
            if (!measurement.valid)
                continue;
            measurements << QStringLiteral("%1=%2 %3")
                                .arg(QString::fromStdString(measurement.name), numberText(measurement.value),
                                     QString::fromStdString(measurement.unit));
        }
        results_->setItem(row, 2, item(measurements.join(QStringLiteral("; "))));
        results_->setItem(
            row, 3, item(index < storedPaths.size() ? QString::fromStdString(storedPaths[index].string()) : QString()));
    }
    if (auto *tabs = findChild<QTabWidget *>())
        tabs->setCurrentIndex(tabs->count() - 1);
}
