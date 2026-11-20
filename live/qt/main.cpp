#include "client.h"
#include <QApplication>
#include <QDir>
#include <QIcon>

extern "C" __declspec(dllexport) int __cdecl run3dsLive(int argc, char **argv, const wchar_t *runtimeDirectory) {
    const QString runtime = QDir::fromNativeSeparators(QString::fromWCharArray(runtimeDirectory));
    QCoreApplication::setLibraryPaths({runtime});
    QDir::setSearchPaths("liveRuntime", {runtime});
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/assets/3ds-live.png")));
    return runClient(app);
}
