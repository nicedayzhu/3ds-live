#pragma once
#include <functional>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
namespace live {
void *createView(const QJsonObject &options);
void setDebugCapture(void *view, std::function<QJsonObject(bool)> capture);
void destroyView(void *view);
QJsonArray takeEvents(void *view);
void showFrame(void *view, const QImage &image, std::function<void()> onPainted = {});
QJsonObject takePaintStats(void *view);
void updateView(void *view, const QJsonObject &values);
} // namespace live
