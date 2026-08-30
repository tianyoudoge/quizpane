#include <QCoreApplication>
#include <QDebug>
#include <QSslSocket>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (!QSslSocket::supportsSsl()) {
        qCritical() << "QSslSocket could not initialize the packaged TLS runtime";
        return 1;
    }

    const QString runtimeVersion = QSslSocket::sslLibraryVersionString();
    qInfo() << "Packaged TLS runtime:" << runtimeVersion;
    if (!runtimeVersion.contains(QStringLiteral("OpenSSL 1.1.1w"))) {
        qCritical() << "Unexpected packaged TLS runtime:" << runtimeVersion;
        return 2;
    }
    return 0;
}
