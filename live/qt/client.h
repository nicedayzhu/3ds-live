#pragma once
#include <QtCore/qglobal.h>
class QApplication;
int runClient(QApplication &app, quint16 handheldDiscoveryPort = 8002);
