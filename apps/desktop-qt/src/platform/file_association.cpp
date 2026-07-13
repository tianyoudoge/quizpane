#include "file_association.hpp"

#if defined(Q_OS_WIN)
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <shellapi.h>
#include <windows.h>
#endif

namespace quizpane::platform {

void registerProviderFileAssociation() {
#if defined(Q_OS_WIN)
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    classes.setValue(QStringLiteral(".quizpane-provider/Default"),
                     QStringLiteral("QuizPane.ProviderPackage"));
    classes.setValue(QStringLiteral("QuizPane.ProviderPackage/Default"),
                     QStringLiteral("小窗刷题题库安装包"));
    classes.setValue(QStringLiteral("QuizPane.ProviderPackage/DefaultIcon/Default"),
                     QStringLiteral("\"%1\",0").arg(executable));
    classes.setValue(QStringLiteral("QuizPane.ProviderPackage/shell/open/command/Default"),
                     QStringLiteral("\"%1\" \"%2\"").arg(executable, QStringLiteral("%1")));
    classes.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

}  // namespace quizpane::platform
