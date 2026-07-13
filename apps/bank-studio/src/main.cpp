#include <QApplication>
#include <QIcon>
#include <QNetworkProxyFactory>

#include "studio_window.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("题库制作器"));
    app.setApplicationDisplayName(QStringLiteral("题库制作器"));
    app.setOrganizationName(QStringLiteral("QuizPane Project"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app-icon.png")));
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    quizpane::studio::StudioWindow window;
    window.show();
    return app.exec();
}
