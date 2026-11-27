#include "waveform_analysis_ui.hpp"

#include "openvlsilib/simulation_store.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <sstream>

namespace {

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

QTableWidgetItem *item(const QString &text, Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable) {
    auto *result = new QTableWidgetItem(text);
    result->setFlags(flags);
    return result;
}

QString numberText(double value) {
    std::ostringstream stream;
    stream.precision(12);
    stream << value;
    return QString::fromStdString(stream.str());
}

QString variantText(const openvlsi::SimulationRunSummary &run) {
    if (!run.variant.empty())
        return QString::fromStdString(run.variant);
    return QString::fromStdString(run.createdAt.empty() ? run.runId : run.createdAt);
}

} // namespace

WaveformAnalysisDialog::WaveformAnalysisDialog(openvlsi::SimulationGraph graph, std::filesystem::path projectRoot,
                                               openvlsi::ObjectId simulationId, QWidget *parent)
    : QDialog(parent), graph_(std::move(graph)), projectRoot_(std::move(projectRoot)), simulationId_(simulationId) {
    setWindowTitle(QStringLiteral("Waveform Analysis — %1").arg(QString::fromStdString(graph_.name)));
    resize(860, 620);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    // View transform and FFT.
    auto *viewPage = new QWidget;
    auto *viewLayout = new QVBoxLayout(viewPage);
    auto *viewForm = new QFormLayout;
    view_ = new QComboBox;
    view_->addItem(QStringLiteral("Linear"), QStringLiteral("linear"));
    view_->addItem(QStringLiteral("Magnitude (dB)"), QStringLiteral("db"));
    view_->addItem(QStringLiteral("Phase"), QStringLiteral("phase"));
    const auto viewIndex = view_->findData(QString::fromStdString(graph_.view));
    view_->setCurrentIndex(viewIndex < 0 ? 0 : viewIndex);
    showMeasurements_ = new QCheckBox(QStringLiteral("Show measurement annotations"));
    showMeasurements_->setChecked(graph_.showMeasurements);
    viewForm->addRow(QStringLiteral("Y view:"), view_);
    viewForm->addRow(QString(), showMeasurements_);
    viewLayout->addLayout(viewForm);

    auto *fftGroup = new QGroupBox(QStringLiteral("FFT view"));
    auto *fftLayout = new QFormLayout(fftGroup);
    fft_ = new QCheckBox(QStringLiteral("Transform the selected traces to the frequency domain"));
    fft_->setChecked(graph_.fft);
    fftWindow_ = new QComboBox;
    fftWindow_->addItems(
        {QStringLiteral("hann"), QStringLiteral("hamming"), QStringLiteral("blackman"), QStringLiteral("none")});
    fftWindow_->setCurrentText(QString::fromStdString(graph_.fftWindow));
    fftPoints_ = new QSpinBox;
    fftPoints_->setRange(0, 1 << 20);
    fftPoints_->setSpecialValueText(QStringLiteral("Automatic (next power of two)"));
    fftPoints_->setValue(graph_.fftPoints);
    fftLayout->addRow(fft_);
    fftLayout->addRow(QStringLiteral("Window:"), fftWindow_);
    fftLayout->addRow(QStringLiteral("FFT size:"), fftPoints_);
    viewLayout->addWidget(fftGroup);
    auto *hint =
        new QLabel(QStringLiteral("The view transform and FFT only change how stored samples are displayed. "
                                  "Phase view uses the companion <signal>.phase traces produced by AC analyses. "
                                  "Non-uniform transient samples are resampled before the FFT."));
    hint->setWordWrap(true);
    viewLayout->addWidget(hint);
    viewLayout->addStretch(1);
    tabs->addTab(viewPage, QStringLiteral("View"));

    // Derived traces.
    auto *derivedPage = new QWidget;
    auto *derivedLayout = new QVBoxLayout(derivedPage);
    derived_ = makeTable({QStringLiteral("Name"), QStringLiteral("Expression")});
    auto *derivedButtons = new QHBoxLayout;
    auto *addDerived = new QPushButton(QStringLiteral("Add"));
    auto *removeDerived = new QPushButton(QStringLiteral("Remove"));
    derivedButtons->addWidget(addDerived);
    derivedButtons->addStretch();
    derivedButtons->addWidget(removeDerived);
    derivedLayout->addWidget(derived_);
    derivedLayout->addLayout(derivedButtons);
    auto *derivedHint =
        new QLabel(QStringLiteral("Expressions combine captured traces with + - * / and abs, sqrt, exp, ln, log10, "
                                  "db, mag, phase, derivative, integral, min, and max. Examples: out - in, "
                                  "db(v(out)), derivative(i(v1)), phase(out)."));
    derivedHint->setWordWrap(true);
    derivedLayout->addWidget(derivedHint);
    connect(addDerived, &QPushButton::clicked, this, [this] { addDerivedRow(); });
    connect(removeDerived, &QPushButton::clicked, this, [this] { removeSelectedRow(derived_); });
    for (const auto &trace : graph_.derivedTraces)
        addDerivedRow(trace);
    tabs->addTab(derivedPage, QStringLiteral("Derived traces"));

    // Overlays.
    auto *overlayPage = new QWidget;
    auto *overlayLayout = new QVBoxLayout(overlayPage);
    overlays_ = makeTable({QStringLiteral("Show"), QStringLiteral("Label"), QStringLiteral("Simulation"),
                           QStringLiteral("Variant"), QStringLiteral("Created"), QStringLiteral("Run")});
    overlayLayout->addWidget(overlays_);
    auto *overlayHint =
        new QLabel(QStringLiteral("Check stored runs to draw them on this graph. This is how corners, sweep "
                                  "points, and Monte Carlo samples are compared without rerunning. Overlaid "
                                  "traces use a dashed line and share the base signal colour."));
    overlayHint->setWordWrap(true);
    overlayLayout->addWidget(overlayHint);
    loadOverlays();
    tabs->addTab(overlayPage, QStringLiteral("Overlays"));

    // Markers.
    auto *markerPage = new QWidget;
    auto *markerLayout = new QVBoxLayout(markerPage);
    markers_ = makeTable({QStringLiteral("Label"), QStringLiteral("Signal"), QStringLiteral("X"), QStringLiteral("Y")});
    auto *markerButtons = new QHBoxLayout;
    auto *addMarker = new QPushButton(QStringLiteral("Add"));
    auto *removeMarker = new QPushButton(QStringLiteral("Remove"));
    markerButtons->addWidget(addMarker);
    markerButtons->addStretch();
    markerButtons->addWidget(removeMarker);
    markerLayout->addWidget(markers_);
    markerLayout->addLayout(markerButtons);
    auto *markerHint =
        new QLabel(QStringLiteral("Markers annotate a point on the waveform. You can also place them directly "
                                  "from the plot context menu. Leave Y blank for a vertical reference line."));
    markerHint->setWordWrap(true);
    markerLayout->addWidget(markerHint);
    connect(addMarker, &QPushButton::clicked, this, [this] { addMarkerRow(); });
    connect(removeMarker, &QPushButton::clicked, this, [this] { removeSelectedRow(markers_); });
    for (const auto &marker : graph_.markers)
        addMarkerRow(marker);
    tabs->addTab(markerPage, QStringLiteral("Markers"));

    layout->addWidget(tabs, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void WaveformAnalysisDialog::accept() {
    if (!apply())
        return;
    QDialog::accept();
}

void WaveformAnalysisDialog::addDerivedRow(const openvlsi::SimulationDerivedTrace &trace) {
    const auto row = derived_->rowCount();
    derived_->insertRow(row);
    derived_->setItem(
        row, 0,
        item(QString::fromStdString(trace.name), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
    derived_->setItem(
        row, 1,
        item(QString::fromStdString(trace.expression), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
}

void WaveformAnalysisDialog::addMarkerRow(const openvlsi::SimulationGraphMarker &marker) {
    const auto row = markers_->rowCount();
    markers_->insertRow(row);
    markers_->setItem(
        row, 0,
        item(QString::fromStdString(marker.label), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
    markers_->setItem(
        row, 1,
        item(QString::fromStdString(marker.signal), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
    markers_->setItem(row, 2,
                      item(numberText(marker.x), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
    markers_->setItem(row, 3,
                      item(marker.hasY ? numberText(marker.y) : QString(),
                           Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
}

void WaveformAnalysisDialog::removeSelectedRow(QTableWidget *table) {
    if (!table)
        return;
    const auto row = table->currentRow();
    if (row >= 0)
        table->removeRow(row);
}

void WaveformAnalysisDialog::loadOverlays() {
    std::set<std::string> included;
    for (const auto &overlay : graph_.overlays)
        included.insert(overlay.runId);
    std::vector<openvlsi::SimulationRunSummary> runs;
    if (!projectRoot_.empty())
        runs = openvlsi::listSimulationRunSummaries(projectRoot_);
    std::set<std::string> listed;
    const auto addRow = [&](const openvlsi::SimulationRunSummary &run, const std::string &label, bool checked) {
        const auto row = overlays_->rowCount();
        overlays_->insertRow(row);
        auto *check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        check->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, QString::fromStdString(run.runId));
        overlays_->setItem(row, 0, check);
        overlays_->setItem(
            row, 1, item(QString::fromStdString(label), Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
        overlays_->setItem(row, 2, item(QString::fromStdString(run.simulationName)));
        overlays_->setItem(row, 3, item(variantText(run)));
        overlays_->setItem(row, 4, item(QString::fromStdString(run.createdAt)));
        overlays_->setItem(row, 5, item(QString::fromStdString(run.runId)));
        listed.insert(run.runId);
    };
    for (const auto &run : runs) {
        std::string label;
        for (const auto &overlay : graph_.overlays) {
            if (overlay.runId != run.runId)
                continue;
            label = overlay.label.empty() ? variantText(run).toStdString() : overlay.label;
            break;
        }
        if (label.empty())
            label = run.variant.empty() ? run.runId : run.variant;
        addRow(run, label, included.contains(run.runId));
    }
    // Keep overlay entries that no longer appear in the run list so the user
    // can see and remove stale selections explicitly.
    for (const auto &overlay : graph_.overlays) {
        if (listed.contains(overlay.runId))
            continue;
        openvlsi::SimulationRunSummary stale;
        stale.runId = overlay.runId;
        stale.createdAt = "missing";
        addRow(stale, overlay.label.empty() ? overlay.runId : overlay.label, true);
    }
}

bool WaveformAnalysisDialog::apply() {
    const auto view = view_->currentData().toString();
    graph_.view = view.isEmpty() ? "linear" : view.toStdString();
    graph_.fft = fft_->isChecked();
    graph_.fftWindow = fftWindow_->currentText().toStdString();
    graph_.fftPoints = fftPoints_->value();
    graph_.showMeasurements = showMeasurements_->isChecked();

    graph_.derivedTraces.clear();
    for (int row = 0; row < derived_->rowCount(); ++row) {
        const auto name = derived_->item(row, 0) ? derived_->item(row, 0)->text().trimmed() : QString();
        const auto expression = derived_->item(row, 1) ? derived_->item(row, 1)->text().trimmed() : QString();
        if (name.isEmpty() && expression.isEmpty())
            continue;
        if (name.isEmpty() || expression.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Derived Trace"),
                                 QStringLiteral("Each derived trace needs a name and an expression."));
            return false;
        }
        graph_.derivedTraces.push_back({name.toStdString(), expression.toStdString()});
    }

    graph_.overlays.clear();
    for (int row = 0; row < overlays_->rowCount(); ++row) {
        const auto *check = overlays_->item(row, 0);
        if (!check || check->checkState() != Qt::Checked)
            continue;
        const auto runId = check->data(Qt::UserRole).toString();
        if (runId.isEmpty())
            continue;
        openvlsi::SimulationOverlayRun overlay;
        overlay.runId = runId.toStdString();
        if (overlays_->item(row, 1))
            overlay.label = overlays_->item(row, 1)->text().trimmed().toStdString();
        graph_.overlays.push_back(std::move(overlay));
    }

    graph_.markers.clear();
    for (int row = 0; row < markers_->rowCount(); ++row) {
        const auto label = markers_->item(row, 0) ? markers_->item(row, 0)->text().trimmed() : QString();
        const auto signal = markers_->item(row, 1) ? markers_->item(row, 1)->text().trimmed() : QString();
        const auto xText = markers_->item(row, 2) ? markers_->item(row, 2)->text().trimmed() : QString();
        const auto yText = markers_->item(row, 3) ? markers_->item(row, 3)->text().trimmed() : QString();
        if (label.isEmpty() && signal.isEmpty() && xText.isEmpty() && yText.isEmpty())
            continue;
        bool ok = false;
        openvlsi::SimulationGraphMarker marker;
        marker.label = label.toStdString();
        marker.signal = signal.toStdString();
        marker.x = xText.toDouble(&ok);
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Marker"),
                                 QStringLiteral("Marker '%1' needs a numeric X value.").arg(label));
            return false;
        }
        if (!yText.isEmpty()) {
            marker.y = yText.toDouble(&ok);
            if (!ok) {
                QMessageBox::warning(this, QStringLiteral("Marker"),
                                     QStringLiteral("Marker '%1' has a non-numeric Y value.").arg(label));
                return false;
            }
            marker.hasY = true;
        }
        graph_.markers.push_back(std::move(marker));
    }
    return true;
}
