#pragma once

#include <QString>

namespace quizpane {

// Use the executable's build architecture, not the host OS architecture:
// an x86 process running on x64 Windows must keep receiving x86 updates.
inline QString updateAssetForPlatform(const QString& platform, bool qt5OrWin7) {
    if (platform == QStringLiteral("windows-x86") ||
        platform == QStringLiteral("windows-x64")) {
        // Only the Win7/Qt5 release pipeline currently publishes x86 artifacts.
        if (platform == QStringLiteral("windows-x86") && !qt5OrWin7)
            return {};
        return QStringLiteral("QuizPane-%1-%2-portable.zip")
            .arg(qt5OrWin7 ? QStringLiteral("windows7") : QStringLiteral("windows"),
                 platform.mid(8));
    }
    if (platform == QStringLiteral("macos-arm64") ||
        platform == QStringLiteral("macos-x86_64"))
        return QStringLiteral("QuizPane-%1.dmg").arg(platform);
    return {};
}

} // namespace quizpane
