// MinerU 版面适配器测试。
//
// 夹具刻意使用合成的 layout.json 而非真实试卷：真题（如公考机构的内部讲义）
// 属第三方版权材料，不适合随开源仓库分发。合成夹具按真实样本观察到的结构
// 复刻了三种关键形态——同一视觉行内的多个选项 span、被 MinerU 归入
// discarded_blocks 的页眉页脚、以及题干与选项分处相邻两页的跨页题——因而对
// 适配器的断言强度与真卷一致。

#include "quizpane/studio/mineru_output_adapter.hpp"
#include "quizpane/zip_archive.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

QByteArray readFixture() {
    QFile file(QStringLiteral(MINERU_LAYOUT_FIXTURE));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace quizpane::studio;

    const QByteArray layout = readFixture();
    if (layout.isEmpty())
        return fail("fixture layout.json unreadable");

    const QString sourcePath = QStringLiteral("/tmp/sample.pdf");
    const MineruAdaptResult adapted = adaptMineruLayout(layout, sourcePath);
    if (!adapted.error.isEmpty())
        return fail("adaptMineruLayout reported an error");
    if (adapted.backend != QStringLiteral("pipeline") ||
        adapted.versionName != QStringLiteral("3.4.4"))
        return fail("MinerU backend/version metadata not captured");

    const ExtractedDocument& document = adapted.document;
    if (document.sourcePath != sourcePath)
        return fail("sourcePath not propagated");
    if (!document.hasPageBoundaries)
        return fail("hasPageBoundaries must be true so source.page can be derived");

    // 与本地 PdfExtractor 同构：换页符分页。
    const QStringList pages = document.plainText.split(QChar(u'\f'));
    if (pages.size() != 2)
        return fail("expected two form-feed separated pages");

    // 页眉页脚位于 discarded_blocks，绝不能进入正文。这是真实样本里
    // “页脚粘进选项行”一类污染的确定性防线。
    if (document.plainText.contains(QStringLiteral("示例页眉")))
        return fail("header text leaked into body text");
    if (document.plainText.contains(QStringLiteral("\n1\n")) ||
        document.plainText.endsWith(QStringLiteral("\n2")))
        return fail("page number leaked into body text");

    // 同一视觉行内的两个选项必须以空格分隔，否则规则引擎无法按标签切分。
    if (!pages.at(0).contains(QStringLiteral("A. 5 B. 4")))
        return fail("same-line options were concatenated without a separator");

    // 跨页题：题干在第 1 页末尾，选项在第 2 页开头，且第 2 页首行就是选项。
    if (!pages.at(0).contains(QStringLiteral("3.在以下信息中，能够推出的有几项？")))
        return fail("cross-page question stem missing from first page");
    if (!pages.at(1).startsWith(QStringLiteral("A. 0项 B. 1项")))
        return fail("cross-page options must lead the second page, free of header text");

    // 题号锚点：第 1 页三题、第 2 页答案区无题号。
    const auto questionsPage1 = document.questionAnchors.value(1);
    if (questionsPage1.size() != 3)
        return fail("expected three question anchors on page 1");
    for (const PdfTextAnchor& anchor : questionsPage1) {
        if (anchor.bounds.isEmpty())
            return fail("question anchor has empty bounds");
        // 归一化到 0..1 是规则引擎的硬约定：本地路径同样如此，两条路径的
        // 锚点必须可以被无差别消费。
        if (anchor.bounds.left() < 0.0 || anchor.bounds.right() > 1.0 ||
            anchor.bounds.top() < 0.0 || anchor.bounds.bottom() > 1.0)
            return fail("question anchor bounds are not normalised to 0..1");
    }

    // 选项标签锚点：每个标签必须有独立坐标。这是选用 layout.json（span 级
    // bbox）而非 content_list.json（段落级 bbox）的核心理由——四个标签共享
    // 一个矩形会让图片/公式选项的裁切失去依据。
    const auto optionsPage1 = document.optionLabelAnchors.value(1);
    if (optionsPage1.size() != 8)
        return fail("expected eight option label anchors on page 1");
    QRectF a1;
    QRectF b1;
    for (const PdfTextAnchor& anchor : optionsPage1) {
        if (anchor.text == QStringLiteral("a") && a1.isEmpty())
            a1 = anchor.bounds;
        if (anchor.text == QStringLiteral("b") && b1.isEmpty())
            b1 = anchor.bounds;
    }
    if (a1.isEmpty() || b1.isEmpty())
        return fail("A/B option anchors missing");
    if (qFuzzyCompare(a1.left(), b1.left()))
        return fail("same-line option labels must not share one bounding box");
    // 同一行的 A 与 B 垂直位置应当接近：规则引擎按 y 差值 <= 0.018 判定同一行。
    if (qAbs(a1.center().y() - b1.center().y()) > 0.018)
        return fail("same-line option labels should share a row");

    // 标签一律小写，与本地路径一致。
    for (const PdfTextAnchor& anchor : optionsPage1) {
        if (anchor.text != anchor.text.toLower())
            return fail("option label anchors must be lower-cased");
    }

    // 行锚点用于材料裁切与下划线定位，必须覆盖正文行且不含页眉。
    const auto linesPage1 = document.lineAnchors.value(1);
    if (linesPage1.isEmpty())
        return fail("line anchors missing on page 1");
    for (const PdfTextAnchor& anchor : linesPage1) {
        if (anchor.text.contains(QStringLiteral("示例页眉")))
            return fail("header text leaked into line anchors");
    }

    // 畸形输入必须显式失败，而不是产出一个"看起来正常但没有内容"的文档。
    if (adaptMineruLayout(QByteArrayLiteral("{not json"), sourcePath).error.isEmpty())
        return fail("malformed JSON must be rejected");
    if (adaptMineruLayout(QByteArrayLiteral("{}"), sourcePath).error.isEmpty())
        return fail("layout without pdf_info must be rejected");
    if (adaptMineruLayout(QByteArray(), sourcePath).error.isEmpty())
        return fail("empty payload must be rejected");

    // ZIP 入口：正常包可读，缺 layout.json 的包必须被拒绝。
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail("temporary directory unavailable");
    const QString zipPath = directory.filePath(QStringLiteral("result.zip"));
    QString zipError;
    if (!quizpane::writeZipArchive(
            zipPath,
            {{QStringLiteral("layout.json"), layout},
             {QStringLiteral("images/chart-a.jpg"), QByteArrayLiteral("not-a-real-jpeg")}},
            &zipError))
        return fail("failed to write test zip");
    const MineruAdaptResult fromZip = adaptMineruZip(zipPath, sourcePath);
    if (!fromZip.error.isEmpty())
        return fail("adaptMineruZip rejected a valid archive");
    if (fromZip.document.questionAnchors.value(1).size() != 3)
        return fail("zip path produced different anchors than direct parsing");

    const QString emptyZipPath = directory.filePath(QStringLiteral("empty.zip"));
    if (!quizpane::writeZipArchive(emptyZipPath,
                                   {{QStringLiteral("full.md"), QByteArrayLiteral("# preview")}},
                                   &zipError))
        return fail("failed to write layout-less zip");
    if (adaptMineruZip(emptyZipPath, sourcePath).error.isEmpty())
        return fail("archive without layout.json must be rejected");

    // 目录入口。
    const QString layoutCopy = directory.filePath(QStringLiteral("layout.json"));
    {
        QFile copy(layoutCopy);
        if (!copy.open(QIODevice::WriteOnly))
            return fail("failed to stage layout.json");
        copy.write(layout);
    }
    if (!adaptMineruDirectory(directory.path(), sourcePath).error.isEmpty())
        return fail("adaptMineruDirectory rejected a valid directory");

    QTemporaryDir emptyDirectory;
    if (!emptyDirectory.isValid())
        return fail("temporary directory unavailable");
    if (adaptMineruDirectory(emptyDirectory.path(), sourcePath).error.isEmpty())
        return fail("directory without layout.json must be rejected");

    std::fprintf(stdout, "mineru_output_adapter_test ok\n");
    return 0;
}
