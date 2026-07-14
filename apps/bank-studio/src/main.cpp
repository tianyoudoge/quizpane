#include <QApplication>
#include <QIcon>
#include <QNetworkProxyFactory>

#include <exception>

#include "quizpane/diagnostic_logger.hpp"
#include "studio_window.hpp"

namespace {
class StudioApplication final : public QApplication {
public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception& error) {
            quizpane::diagnostic::event(QStringLiteral("qt"),
                QStringLiteral("unhandled-event-exception"),
                {{QStringLiteral("receiver"), receiver ? receiver->metaObject()->className() : "null"},
                 {QStringLiteral("eventType"), event ? static_cast<int>(event->type()) : -1},
                 {QStringLiteral("error"), QString::fromLocal8Bit(error.what())}});
            throw;
        } catch (...) {
            quizpane::diagnostic::event(QStringLiteral("qt"),
                QStringLiteral("unhandled-event-exception"),
                {{QStringLiteral("receiver"), receiver ? receiver->metaObject()->className() : "null"},
                 {QStringLiteral("eventType"), event ? static_cast<int>(event->type()) : -1},
                 {QStringLiteral("error"), QStringLiteral("unknown")}});
            throw;
        }
    }
};
} // namespace

int main(int argc, char* argv[]) {
    StudioApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("题库制作器"));
    app.setApplicationDisplayName(QStringLiteral("题库制作器"));
    app.setApplicationVersion(QStringLiteral(QUIZPANE_VERSION));
    app.setOrganizationName(QStringLiteral("QuizPane Project"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app-icon.png")));
    quizpane::diagnostic::initialize(QStringLiteral("question-maker"));
    quizpane::diagnostic::event(QStringLiteral("app"), QStringLiteral("initialized"),
        {{QStringLiteral("arguments"), argc}});
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    quizpane::studio::StudioWindow window;
    window.show();
    const int exitCode = app.exec();
    quizpane::diagnostic::event(QStringLiteral("app"), QStringLiteral("event-loop-returned"),
        {{QStringLiteral("code"), exitCode}});
    quizpane::diagnostic::shutdown();
    return exitCode;
}
