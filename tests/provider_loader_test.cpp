#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>

#include <cstdlib>
#include <iostream>

#include "quizpane/provider_loader.hpp"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    quizpane::ProviderLoader loader;
    QString error;
    const QString providerPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                          : QString::fromUtf8(DEMO_PROVIDER_PATH);
    if (!loader.load(providerPath, &error)) {
        std::cerr << error.toStdString() << '\n';
        return EXIT_FAILURE;
    }
    const QString expectedId = loader.descriptor().value("id").toString();
    if (expectedId.isEmpty()) return EXIT_FAILURE;

    QEventLoop loop;
    bool success = false;
    QObject::connect(&loader, &quizpane::ProviderLoader::responseReceived, &loop,
                     [&](const QJsonObject& response) {
                         success = response.value("result")
                                       .toObject()
                                       .value("providerId") == expectedId;
                         loop.quit();
                     });
    loader.request({{"id", "test-1"}, {"method", "provider.initialize"}},
                   &error);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
