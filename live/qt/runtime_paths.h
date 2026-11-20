#pragma once
#include <QCoreApplication>
#include <QDir>

namespace live {
inline QDir runtimeDirectory() {
    const auto paths = QDir::searchPaths("liveRuntime");
    // Standalone development tools have their models beside their executable.
    return QDir(paths.isEmpty() ? QCoreApplication::applicationDirPath() : paths.first());
}
}
