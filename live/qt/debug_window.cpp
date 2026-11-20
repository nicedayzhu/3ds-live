#include "debug_window.h"
namespace live {
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
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs);
    samples = new QTableWidget(0, 7, this);
    samples->setHorizontalHeaderLabels(
        {"时间", "收到 FPS", "呈现 FPS", "最长间隔 ms", "增强覆盖", "实际 MB/s", "掌机 FPS"});
    samples->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    samples->verticalHeader()->hide();
    samples->setEditTriggers(QAbstractItemView::NoEditTriggers);
    samples->setSelectionBehavior(QAbstractItemView::SelectRows);
    tabs->addTab(samples, "最近 60 秒");
    raw = new QPlainTextEdit(this);
    raw->setReadOnly(true);
    raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs->addTab(raw, "原始数据");
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
    resize(750, 510);
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
    auto rows = freeze->isChecked() ? frozen["samples"].toArray() : history;
    auto snap = freeze->isChecked() ? frozen["snapshot"].toObject() : latest;
    int scroll = raw->verticalScrollBar()->value();
    raw->setPlainText(QString::fromUtf8(QJsonDocument(snap).toJson()));
    raw->verticalScrollBar()->setValue(scroll);
    samples->setUpdatesEnabled(false);
    samples->setRowCount(int(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        auto row = rows[rows.size() - i - 1].toObject();
        QStringList cells = {row["time"].toString(),
                             QString::number(row["fps_received"].toDouble(), 'f', 1),
                             QString::number(row["fps_presented"].toDouble(), 'f', 1),
                             QString::number(row["interval_max_gap_ms"].toDouble(), 'f', 1),
                             row["coverage_pct"].isDouble()
                                 ? QString::number(row["coverage_pct"].toDouble(), 'f', 0) + "%"
                                 : QStringLiteral("—"),
                             QString::number(row["bytes_per_s"].toDouble() / 1e6, 'f', 2),
                             QString::number(row["capture_fps"].toInt())};
        for (int j = 0; j < cells.size(); ++j) {
            auto *item = samples->item(i, j);
            if (!item) {
                item = new QTableWidgetItem;
                samples->setItem(i, j, item);
            }
            item->setText(cells[j]);
        }
    }
    samples->setUpdatesEnabled(true);
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
