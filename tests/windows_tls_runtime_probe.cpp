#include <QCoreApplication>
#include <QDebug>
#include <QSslSocket>
#include <QtGlobal>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QStringList backends = QSslSocket::availableBackends();
    if (!backends.contains(QStringLiteral("schannel"))) {
        qCritical() << "Packaged Qt TLS plugins do not provide Schannel:" << backends;
        return 1;
    }
    if (!QSslSocket::setActiveBackend(QStringLiteral("schannel"))) {
        qCritical() << "Could not activate the packaged Schannel backend";
        return 2;
    }
#endif

    if (!QSslSocket::supportsSsl()) {
        qCritical() << "QSslSocket could not initialize the packaged TLS runtime";
        return 3;
    }

    const QString runtimeVersion = QSslSocket::sslLibraryVersionString();
    qInfo() << "Packaged TLS runtime:" << runtimeVersion;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!runtimeVersion.contains(QStringLiteral("OpenSSL 1.1.1w"))) {
        qCritical() << "Unexpected packaged TLS runtime:" << runtimeVersion;
        return 4;
    }
#endif
    return 0;
}
