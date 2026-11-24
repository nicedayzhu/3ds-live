#include "debug_window.h"
namespace live {
namespace {
constexpr const char *columns[] = {"时间", "采样窗口", "掌机提交 FPS", "双屏收齐 FPS", "上屏收齐 FPS", "上屏源呈现 FPS",
    "掌机上限 MB/s", "掌机提交 MB/s", "PC 实收 MB/s", "本秒 Q 上/下", "本秒最长间隔 ms", "本秒增强覆盖"};
}

class DebugSamples final : public QAbstractTableModel {
    QJsonArray rows;
  public:
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(std::size(columns)); }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || section < 0 || section >= columnCount()) return {};
        if (role == Qt::DisplayRole) return QString::fromUtf8(columns[section]);
        if (role == Qt::ToolTipRole) {
            switch (section) {
            case 1: return QStringLiteral("两端以同一组掌机状态包划定区间。状态不足时仅显示本机采样，原始逐秒计数见原始数据");
            case 2: return QStringLiteral("掌机网络栈成功接收的双屏帧对数，按掌机状态计数差计算");
            case 3: return QStringLiteral("PC 完整收到并组好上下两屏的帧对数");
            case 5: return QStringLiteral("实际选择呈现的上屏源帧，排除生成帧。播放缓冲可让个别帧跨越窗口边界");
            case 6: return QStringLiteral("掌机确认的 IPv4 带宽上限，1 MB = 1,000,000 字节");
            case 7: return QStringLiteral("窗口内成功交给掌机网络栈的音视频 IPv4 字节，含纠错和分片头");
            case 8: return QStringLiteral("对齐窗口统计 PC 收到的同类音视频 IPv4 字节；本机采样值另含少量状态包");
            }
        }
        return {};
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole) return {};
        const auto row = rows.at(rows.size()-1-index.row()).toObject();
        const auto window = row.value("delivery_window").toObject();
        const auto format = [](QJsonValue v, int digits, double divisor = 1.) {
            return v.isDouble() ? QString::number(v.toDouble()/divisor, 'f', digits) : QStringLiteral("—");
        };
        const auto measured = [&](const char *aligned, const char *local) {
            return window.isEmpty() ? row.value(local) : window.value(aligned);
        };
        const bool fresh = row.contains("device_status_us") &&
            row.value("wall_us").toInteger() - row.value("device_status_us").toInteger() < 2500000;
        switch (index.column()) {
        case 0: return row.value("time").toString();
        case 1: return window.isEmpty() ? QStringLiteral("本机") : format(window.value("seconds"),1) + " s";
        case 2: return format(window.value("device_sent_fps"),1);
        case 3: return format(measured("pair_fps","fps_received"),1);
        case 4: return format(measured("top_fps","fps_top_received"),1);
        case 5: return format(measured("presented_top_fps","fps_source_presented"),1);
        case 6: return !window.isEmpty() || fresh ? format(measured("device_cap","device_cap"),2,1e6) : QStringLiteral("—");
        case 7: return format(window.value("device_MBps"),2);
        case 8: return window.isEmpty() ? format(row.value("bytes_per_s"),2,1e6) : format(window.value("received_MBps"),2);
        case 9: return format(row.value("jpeg_top_q"),0) + "/" + format(row.value("jpeg_bottom_q"),0);
        case 10: return format(row.value("interval_max_gap_ms"),1);
        case 11: return row.value("coverage_pct").isDouble() ? format(row.value("coverage_pct"),0) + "%" : QStringLiteral("—");
        }
        return {};
    }
    void replace(const QJsonArray &history) {
        if (rows.size() == history.size()) {
            rows = history;
            if (!rows.isEmpty()) emit dataChanged(index(0,0), index(rowCount()-1,columnCount()-1), {Qt::DisplayRole});
        } else {
            beginResetModel();
            rows = history;
            endResetModel();
        }
    }
};
DebugWindow::DebugWindow(QWidget *parent, DebugCapture provider)
    : QDialog(parent), capture(std::move(provider)) {
    setWindowTitle("3DS Live · 开发者调试");
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto *mark = new QPushButton("刚才卡了一下", this);
    mark->setObjectName("markStall");
    bar->addWidget(mark);
    freeze = new QCheckBox("冻结", this);
    bar->addWidget(freeze);
    auto *save = new QPushButton("导出诊断…", this);
    save->setObjectName("exportDebug");
    bar->addWidget(save);
    auto *copy = new QPushButton("复制 JSON", this);
    bar->addWidget(copy);
    logs = new QPushButton("统计目录", this);
    bar->addWidget(logs);
    bar->addStretch();
    marker = new QLabel(this);
    layout->addWidget(marker);
    tabs = new QTabWidget(this);
    layout->addWidget(tabs);
    auto *table = new QTableView(this);
    samples = new DebugSamples(table);
    table->setModel(samples);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Stable, user-resizable widths keep history refreshes off the geometry path.
    auto *header = table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    for (int i=0; i<samples->columnCount(); ++i)
        header->resizeSection(i, std::max(header->sectionSizeHint(i), table->fontMetrics().horizontalAdvance("00:00:00") + 18));
    tabs->addTab(table, "最近 60 秒");
    raw = new QPlainTextEdit(this);
    raw->setReadOnly(true);
    raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(raw, "原始数据");
    connect(tabs, &QTabWidget::currentChanged, this, [this] { render(); });
    connect(mark, &QPushButton::clicked, this, [this] {
        auto captured = archive(true);
        QSignalBlocker block(freeze);
        freeze->setChecked(true);
        frozen = std::move(captured);
        marker->setText("已标记卡顿并冻结最近 60 秒");
        render();
    });
    connect(freeze, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            frozen = archive();
        else {
            frozen = {};
            marker->clear();
        }
        render();
    });
    connect(save, &QPushButton::clicked, this, [this] { exportFile(); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(
            QString::fromUtf8(QJsonDocument(freeze->isChecked() ? frozen : archive()).toJson()));
    });
    connect(logs, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(latest["stats_file"].toString()).absolutePath()));
    });
    resize(1020, 510);
}
QJsonObject DebugWindow::archive(bool mark) const {
    if (capture)
        return capture(mark);
    return {{"snapshot", latest}, {"samples", history}};
}
void DebugWindow::receive(const QJsonObject &snapshot, const QJsonArray &rows) {
    latest = snapshot;
    history = rows;
    logs->setEnabled(!latest["stats_file"].toString().isEmpty());
    if (isVisible() && !freeze->isChecked())
        render();
}
void DebugWindow::render() {
    if (tabs->currentWidget() != raw) {
        samples->replace(freeze->isChecked() ? frozen.value("samples").toArray() : history);
        return;
    }
    const auto snap = freeze->isChecked() ? frozen.value("snapshot").toObject() : latest;
    int scroll = raw->verticalScrollBar()->value();
    raw->setPlainText(QString::fromUtf8(QJsonDocument(snap).toJson()));
    raw->verticalScrollBar()->setValue(scroll);
}
void DebugWindow::exportFile() {
    const auto captured = freeze->isChecked() ? frozen : archive();
    auto *dialog = new QFileDialog(
        this, "导出最近 60 秒诊断",
        "3DS-Live-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".json", "JSON (*.json)");
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix("json");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QFileDialog::fileSelected, this, [this, captured](const QString &path) {
        QSaveFile out(path);
        auto bytes = QJsonDocument(captured).toJson(QJsonDocument::Compact);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
            marker->setText("导出失败 · " + out.errorString());
        else
            marker->setText("已导出至 " + QDir::toNativeSeparators(path));
    });
    dialog->open();
}
} // namespace live
