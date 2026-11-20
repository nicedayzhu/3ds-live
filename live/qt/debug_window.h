#pragma once
#include <QtWidgets>
#include <functional>
namespace live {
using DebugCapture = std::function<QJsonObject(bool)>;
class DebugWindow final : public QDialog {
    DebugCapture capture;
    QCheckBox *freeze;
    QTableWidget *samples;
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
