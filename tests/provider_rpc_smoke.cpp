#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QTextStream>

#include "quizpane/provider_loader.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) return 64;
    quizpane::ProviderLoader loader;
    QString error;
    if (!loader.load(QString::fromLocal8Bit(argv[1]), &error)) return 1;

    const QString method = QString::fromUtf8(argv[2]);
    QEventLoop loop;
    bool success = false;
    QObject::connect(&loader, &quizpane::ProviderLoader::responseReceived, &loop,
                     [&](const QJsonObject& response) {
        if (response.contains("error")) {
            const auto rpcError = response.value("error").toObject();
            QTextStream(stderr) << rpcError.value("code").toInt() << ": "
                                << rpcError.value("message").toString() << '\n';
        }
        if (method == QStringLiteral("auth.begin")) {
            const auto result = response.value("result").toObject();
            success = !result.value("loginSessionId").toString().isEmpty() &&
                      !result.value("qrContent").toString().isEmpty();
        } else {
            success = response.contains("result");
        }
        loop.quit();
    });
    if (!loader.request({{"id", "smoke-1"}, {"method", method},
                         {"params", QJsonObject{}}}, &error)) return 2;
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec();
    return success ? 0 : 3;
}
