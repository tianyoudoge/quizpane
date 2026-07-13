#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QNetworkProxyFactory>

#include "ui/main_window.hpp"

int main(int argc, char* argv[]) {
    // QApplication 是 Qt Widgets 的进程级运行时，角色接近浏览器的 window +
    // event loop。所有控件必须在它创建之后、app.exec() 之前构造。
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("小窗刷题"));
    QApplication::setApplicationDisplayName(QStringLiteral("小窗刷题"));
    QApplication::setApplicationVersion(QStringLiteral(QUIZPANE_VERSION));
    QApplication::setOrganizationName("QuizPane Project");
    QApplication::setDesktopFileName(QStringLiteral("org.quizpane.app"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app-icon.png")));
    // Host 显式初始化 QtNetwork：一方面让 Provider 继承系统代理设置，另一方面
    // 保证 macdeployqt/windeployqt 在没有内置在线题库时也会打包网络运行库。
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    QApplication::setQuitOnLastWindowClosed(false);

    // 命令行入口主要服务开发调试和“双击安装包”场景；普通用户通过界面菜单操作。
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("小窗刷题桌面客户端"));
    parser.addHelpOption();
    QCommandLineOption providerOption(
        {"p", "provider"}, QStringLiteral("加载题库扩展动态库"), "path");
    parser.addOption(providerOption);
    parser.addPositionalArgument(
        QStringLiteral("package"),
        QStringLiteral("要导入的题库安装包"),
        QStringLiteral("[package]"));
    parser.process(app);

    quizpane::MainWindow window;
    window.show();
    if (parser.isSet(providerOption))
        window.loadProvider(parser.value(providerOption));
    else if (!parser.positionalArguments().isEmpty())
        window.installProviderPackage(parser.positionalArguments().first());
    else
        window.loadLastProvider();
    // exec() 进入 UI 事件循环，直到托盘菜单选择“退出程序”才返回。
    return app.exec();
}
