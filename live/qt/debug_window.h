#pragma once
#include <QtWidgets>
#include <functional>
namespace live {
class DebugSamples;
using DebugCapture = std::function<QJsonObject(bool)>;
class DebugWindow final : public QDialog {
    DebugCapture capture;
    QCheckBox *freeze;
    QTabWidget *tabs;
    DebugSamples *samples;
    QPlainTextEdit *raw;
    QPushButton *logs;
    QLabel *marker;
    QJsonObject latest, frozen;
    QJsonArray history;
    QJsonObject archive(bool mark = false) const;
    void render();
    void exportFile();

  public:
    explicit DebugWindow(QWidget *parent, DebugCapture capture);
    void receive(const QJsonObject &snapshot, const QJsonArray &rows);
};
} // namespace live
