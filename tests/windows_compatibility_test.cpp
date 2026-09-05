#include "../apps/desktop-qt/src/platform/update_asset.hpp"
#include "../apps/bank-studio/engine/include/quizpane/studio/pdf_memory_budget.hpp"
#include "quizpane/provider_installer.hpp"
#include <cstdio>

int main() {
    using quizpane::updateAssetForPlatform;
    if (updateAssetForPlatform("windows-x86", true) != "QuizPane-windows7-x86-portable.zip" ||
        updateAssetForPlatform("windows-x64", true) != "QuizPane-windows7-x64-portable.zip" ||
        updateAssetForPlatform("windows-x64", false) != "QuizPane-windows-x64-portable.zip" ||
        !updateAssetForPlatform("windows-x86", false).isEmpty() ||
        !updateAssetForPlatform("windows-arm64", true).isEmpty() ||
        !updateAssetForPlatform("unknown", false).isEmpty() ||
        updateAssetForPlatform("macos-arm64", false) != "QuizPane-macos-arm64.dmg" ||
        updateAssetForPlatform("macos-x86_64", false) != "QuizPane-macos-x86_64.dmg")
        return 1;
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_32)
    if (quizpane::ProviderInstaller::currentPlatformKey() != "windows-x86") return 2;
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    if (quizpane::ProviderInstaller::currentPlatformKey() != "windows-x64") return 3;
#endif
    using namespace quizpane::studio;
    constexpr quint64 mib = 1024ULL * 1024;
    // Small Win7 machine: less than the old fixed 2 GiB commit reserve, but
    // enough space for an ordinary preview, must not lose all image support.
    PdfMemorySnapshot memory{2048 * mib, 800 * mib, 900 * mib, 1100 * mib, 400 * mib, 60};
    if (!pdfMemoryBudgetAllows(memory, 32 * mib, true)) return 4;
    memory.totalPhysical = 4096 * mib;
    memory.privateBytes = 1180 * mib;
    if (pdfMemoryBudgetAllows(memory, 32 * mib, true)) return 5;
    memory.privateBytes = 400 * mib;
    memory.availableVirtual = 140 * mib;
    if (pdfMemoryBudgetAllows(memory, 32 * mib, true)) return 6;
    memory.availableVirtual = 1100 * mib;
    memory.availableCommit = 520 * mib;
    if (pdfMemoryBudgetAllows(memory, 32 * mib, true)) return 7;
    memory.availableCommit = 900 * mib;
    memory.memoryLoad = 96;
    memory.availablePhysical = 100 * mib;
    if (pdfMemoryBudgetAllows(memory, 32 * mib, false)) return 8;
    std::puts("windows_compatibility_test ok");
    return 0;
}
