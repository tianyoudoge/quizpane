#pragma once

#include <QSize>
#include <QtGlobal>
#include <algorithm>

namespace quizpane::studio {

struct PdfMemorySnapshot {
    quint64 totalPhysical = 0;
    quint64 availablePhysical = 0;
    quint64 availableCommit = 0;
    quint64 availableVirtual = 0;
    quint64 privateBytes = 0;
    unsigned memoryLoad = 0;
};

// Initial conservative budgets; opaque PDFium/OCR allocations and address
// fragmentation still require target-machine validation.
inline bool pdfMemoryBudgetAllows(const PdfMemorySnapshot& memory,
                                 quint64 imageBytes, bool process32Bit) {
    constexpr quint64 mib = 1024ULL * 1024;
    const quint64 privateLimit = process32Bit ? 1200 * mib :
        std::max<quint64>(1536 * mib, memory.totalPhysical * 3 / 4);
    const quint64 commitReserve = std::clamp<quint64>(memory.totalPhysical / 8,
                                                     128 * mib, 512 * mib);
    return memory.privateBytes < privateLimit &&
        imageBytes <= privateLimit - memory.privateBytes &&
        memory.availableCommit > commitReserve &&
        imageBytes <= memory.availableCommit - commitReserve &&
        memory.availableVirtual > 128 * mib &&
        imageBytes <= memory.availableVirtual - 128 * mib &&
        !(memory.memoryLoad >= 94 &&
          memory.availablePhysical <= std::max<quint64>(256 * mib, imageBytes));
}

bool pdfImageAllocationAllowed(const QSize& pixels);

} // namespace quizpane::studio
