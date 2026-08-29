#include "quizpane/studio/mineru_output_adapter.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <miniz.h>

#include <limits>

namespace quizpane::studio {
namespace {

// MinerU 结果来自网络，按不可信输入处理。上限取值远高于真实试卷（十页卷的
// layout.json 约 460 KiB），只用于挡住畸形/恶意包，不影响正常使用。
constexpr qint64 kMaxLayoutJsonBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaxArchiveBytes = 512LL * 1024 * 1024;

QString joinSpanText(const QJsonObject& line) {
    QString text;
    const QJsonArray spans = line.value(QStringLiteral("spans")).toArray();
    for (const QJsonValue& value : spans) {
        const QJsonObject span = value.toObject();
        if (span.value(QStringLiteral("type")).toString() != QStringLiteral("text"))
            continue;
        const QString content = span.value(QStringLiteral("content")).toString();
        if (content.isEmpty())
            continue;
        // MinerU 的同一行 span 之间不含空格；直接相接会把 "A. 甲" "B. 乙"
        // 粘成 "A. 甲B. 乙"。补一个空格既保持人类可读，也让规则引擎的
        // optionsOnLine() 能按标签切分。
        if (!text.isEmpty())
            text.append(QChar(u' '));
        text.append(content);
    }
    return text;
}

// 少数双栏/题卡式 PDF 会让 MinerU 把左侧题号排到本视觉行的末尾。例如原卷
// ``1. 借景是……建筑`` 会被还原成 ``借景是……建筑1.``。题目正文仍在，
// 但后续规则引擎只接受行首题号，因而会误报“没有识别到题号锚点”。
//
// 仅修复“足够长的正文 + 行末独立题号”这一窄形态：选项 ``A. 1.``、小数
// 或普通短句末尾的数字不会命中。保留一个合成题号行及其原始整行 bbox，令
// 下游的视觉裁切仍有可用锚点；span 内已经丢失题号的精确位置，不能伪造它。
QString repairTrailingQuestionNumber(const QString& text, QString* number) {
    static const QRegularExpression trailingQuestion(QStringLiteral(
        R"(^\s*(.{12,}?)\s*(\d{1,4})\s*[.．、]\s*$)"));
    const auto match = trailingQuestion.match(text);
    if (!match.hasMatch())
        return text;
    const QString stem = match.captured(1).trimmed();
    const int value = match.captured(2).toInt();
    if (stem.isEmpty() || value <= 0)
        return text;
    if (number)
        *number = match.captured(2);
    return QStringLiteral("%1. %2").arg(match.captured(2), stem);
}

// MinerU 的 bbox 是页面坐标（单位与 page_size 一致），ExtractedDocument 要求
// 归一化到 0..1——本地 PdfExtractor 走的是同一套约定，因而两条路径产出的锚点
// 可以被规则引擎无差别消费。
QRectF normalizedBounds(const QJsonArray& bbox, const QSizeF& pageSize) {
    if (bbox.size() < 4 || pageSize.width() <= 0.0 || pageSize.height() <= 0.0)
        return {};
    const double x0 = bbox.at(0).toDouble();
    const double y0 = bbox.at(1).toDouble();
    const double x1 = bbox.at(2).toDouble();
    const double y1 = bbox.at(3).toDouble();
    const double left = qMin(x0, x1) / pageSize.width();
    const double top = qMin(y0, y1) / pageSize.height();
    const double width = qAbs(x1 - x0) / pageSize.width();
    const double height = qAbs(y1 - y0) / pageSize.height();
    if (width <= 0.0 || height <= 0.0)
        return {};
    return {left, top, width, height};
}

QSizeF pageSizeOf(const QJsonObject& page) {
    const QJsonArray size = page.value(QStringLiteral("page_size")).toArray();
    if (size.size() < 2)
        return {};
    return {size.at(0).toDouble(), size.at(1).toDouble()};
}

// 逐 span 采集题号与选项标签锚点。
//
// 这是选择 layout.json 而非 content_list.json 的核心理由：真题的四个选项常排
// 在同一视觉行内，只有 span 级坐标才能给每个标签单独定位。段落级 bbox 会让
// A/B/C/D 共享同一个矩形，图片与公式选项的裁切随即失效。
void collectAnchorsFromSpans(const QJsonObject& line, int pageNumber, const QSizeF& pageSize,
                             ExtractedDocument* result) {
    static const QRegularExpression questionMarker(
        QStringLiteral(R"(^\s*(\d{1,4})\s*[、.．])"));
    // 与本地路径保持一致：只认 A-D，避免把正文里的字母误当选项标签。
    static const QRegularExpression optionMarker(
        QStringLiteral(R"((?<![A-Za-z0-9])([A-D])\s*[、.．])"));

    const QJsonArray spans = line.value(QStringLiteral("spans")).toArray();
    for (const QJsonValue& value : spans) {
        const QJsonObject span = value.toObject();
        if (span.value(QStringLiteral("type")).toString() != QStringLiteral("text"))
            continue;
        const QString content = span.value(QStringLiteral("content")).toString();
        if (content.isEmpty())
            continue;
        const QRectF bounds =
            normalizedBounds(span.value(QStringLiteral("bbox")).toArray(), pageSize);
        if (bounds.isEmpty())
            continue;

        const auto question = questionMarker.match(content);
        if (question.hasMatch())
            result->questionAnchors[pageNumber].append({question.captured(1), bounds});

        // 一个 span 内可能仍有多个标签（MinerU 偶尔把 "A. 甲 B. 乙" 合成一个
        // span）。此时四个标签只能共享该 span 的坐标；规则引擎要求同一行标签
        // y 值接近，这一点仍然满足，只是 x 精度下降。宁可略粗也不要丢标签：
        // 丢标签会让图片选项题直接失去裁切依据。
        auto options = optionMarker.globalMatch(content);
        while (options.hasNext()) {
            const auto match = options.next();
            result->optionLabelAnchors[pageNumber].append(
                {match.captured(1).toLower(), bounds});
        }
    }
}

struct PageText {
    QString text;
    bool hasContent = false;
};

// 把一页 para_blocks 还原成纯文本并采集锚点。
// discarded_blocks（页眉、页脚、页码）刻意不参与：它们在 MinerU 输出里已与正文
// 结构性分离，这正是 Day11 第 130 题“页脚粘进选项行”的确定性解法——无需靠
// 文案黑名单去猜。
PageText buildPageText(const QJsonObject& page, int pageNumber, const QSizeF& pageSize,
                       ExtractedDocument* result) {
    PageText out;
    QStringList lines;
    const QJsonArray paragraphs = page.value(QStringLiteral("para_blocks")).toArray();
    for (const QJsonValue& blockValue : paragraphs) {
        const QJsonObject block = blockValue.toObject();
        const QString type = block.value(QStringLiteral("type")).toString();

        // image/chart/table 是嵌套结构（外层 bbox + 内层 blocks），其 span 承载
        // 的是 image_path 而非文字。这里只记录版面存在性：最终裁图仍由规则引擎
        // 按 bbox 从原卷高分辨率渲染图切取，不使用 MinerU 导出的压缩图。
        QJsonArray lineArray = block.value(QStringLiteral("lines")).toArray();
        if (lineArray.isEmpty()) {
            const QJsonArray nested = block.value(QStringLiteral("blocks")).toArray();
            for (const QJsonValue& nestedValue : nested) {
                const QJsonArray nestedLines =
                    nestedValue.toObject().value(QStringLiteral("lines")).toArray();
                for (const QJsonValue& nestedLine : nestedLines)
                    lineArray.append(nestedLine);
            }
        }

        for (const QJsonValue& lineValue : lineArray) {
            const QJsonObject line = lineValue.toObject();
            const QString rawText = joinSpanText(line);
            QString repairedQuestionNumber;
            const QString text = repairTrailingQuestionNumber(rawText, &repairedQuestionNumber);
            if (text.trimmed().isEmpty())
                continue;
            const QRectF bounds =
                normalizedBounds(line.value(QStringLiteral("bbox")).toArray(), pageSize);
            if (!bounds.isEmpty())
                result->lineAnchors[pageNumber].append({text, bounds});
            collectAnchorsFromSpans(line, pageNumber, pageSize, result);
            if (!repairedQuestionNumber.isEmpty() && !bounds.isEmpty())
                result->questionAnchors[pageNumber].append({repairedQuestionNumber, bounds});
            lines.append(text);
        }
        Q_UNUSED(type)
    }
    out.text = lines.join(QChar(u'\n'));
    out.hasContent = !lines.isEmpty();
    return out;
}

bool isSafeZipEntryPath(const QString& path) {
    if (path.isEmpty())
        return false;
    if (QDir::isAbsolutePath(path) || path.startsWith(QChar(u'/')) || path.contains(QStringLiteral("..")))
        return false;
    // Windows 盘符与 UNC 路径同样拒绝：ZIP 内容来自网络，不得写出到任意位置。
    if (path.size() > 1 && path.at(1) == QChar(u':'))
        return false;
    if (path.startsWith(QStringLiteral("\\\\")))
        return false;
    return true;
}

// 从 MinerU 结果 ZIP 中取出 layout.json。
//
// 直接用 miniz 而非 sdk 的 ZipArchiveReader：engine 是不依赖 provider_host 的
// 静态库（DocxExtractor 同样直接使用 miniz），为读一个条目引入跨库依赖并不划算。
//
// 这些包来自网络，因而在解压前逐条校验路径并累计体积：拒绝绝对路径、`..`
// 穿越与超限包，避免 ZIP Slip 和压缩炸弹。适配器只读取 layout.json 到内存、
// 不向磁盘写出任何条目，所以路径校验是纵深防御而非唯一防线。
QByteArray readLayoutJsonFromZip(const QString& zipPath, QString* error) {
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开 MinerU 结果包：%1").arg(file.errorString());
        return {};
    }
    if (file.size() <= 0 || file.size() > kMaxArchiveBytes) {
        *error = QStringLiteral("MinerU 结果包为空或体积超限");
        return {};
    }
    const QByteArray archiveBytes = file.readAll();
    mz_zip_archive archive{};
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_mem(&archive, archiveBytes.constData(),
                                static_cast<size_t>(archiveBytes.size()), 0)) {
        *error = QStringLiteral("MinerU 结果包 ZIP 结构无效");
        return {};
    }

    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    mz_uint64 totalUncompressed = 0;
    int layoutIndex = -1;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
            mz_zip_reader_end(&archive);
            *error = QStringLiteral("MinerU 结果包条目无法读取");
            return {};
        }
        const QString entryPath = QString::fromUtf8(stat.m_filename);
        if (mz_zip_reader_is_file_a_directory(&archive, index))
            continue;
        if (!isSafeZipEntryPath(entryPath)) {
            mz_zip_reader_end(&archive);
            *error = QStringLiteral("MinerU 结果包包含非法路径，已拒绝");
            return {};
        }
        totalUncompressed += stat.m_uncomp_size;
        if (totalUncompressed > static_cast<mz_uint64>(kMaxArchiveBytes)) {
            mz_zip_reader_end(&archive);
            *error = QStringLiteral("MinerU 结果包解压体积超限，已拒绝");
            return {};
        }
        // layout.json 可能位于任意子目录，按后缀匹配而不是写死路径。
        if (layoutIndex < 0 && (entryPath == QStringLiteral("layout.json") ||
                                entryPath.endsWith(QStringLiteral("/layout.json")))) {
            if (stat.m_uncomp_size > static_cast<mz_uint64>(kMaxLayoutJsonBytes) ||
                stat.m_uncomp_size >
                    static_cast<mz_uint64>((std::numeric_limits<qsizetype>::max)())) {
                mz_zip_reader_end(&archive);
                *error = QStringLiteral("MinerU 版面数据过大，已拒绝解析");
                return {};
            }
            layoutIndex = static_cast<int>(index);
        }
    }

    if (layoutIndex < 0) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("MinerU 结果包缺少 layout.json");
        return {};
    }

    size_t size = 0;
    void* bytes =
        mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(layoutIndex), &size, 0);
    // 与 DOCX 路径同理：失败时返回空指针而 size 可能非零，必须按失败处理，
    // 不能构造一个空 QByteArray 当作成功。
    if (!bytes) {
        mz_zip_reader_end(&archive);
        *error = QStringLiteral("MinerU 版面数据解压失败");
        return {};
    }
    QByteArray result(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    mz_free(bytes);
    mz_zip_reader_end(&archive);
    return result;
}

} // namespace

MineruAdaptResult adaptMineruLayout(const QByteArray& layoutJson, const QString& sourcePath,
                                    const MineruParseOptions& options) {
    MineruAdaptResult result;
    result.document.sourcePath = sourcePath;

    if (layoutJson.isEmpty()) {
        result.error = QStringLiteral("MinerU 解析结果为空");
        return result;
    }
    if (layoutJson.size() > kMaxLayoutJsonBytes) {
        result.error = QStringLiteral("MinerU 版面数据过大，已拒绝解析");
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(layoutJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("MinerU 版面数据不是合法 JSON：%1").arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = document.object();
    result.backend = root.value(QStringLiteral("_backend")).toString();
    result.versionName = root.value(QStringLiteral("_version_name")).toString();

    const QJsonArray pages = root.value(QStringLiteral("pdf_info")).toArray();
    if (pages.isEmpty()) {
        result.error = QStringLiteral("MinerU 版面数据不包含任何页面");
        return result;
    }

    QStringList pageTexts;
    bool anyContent = false;
    for (const QJsonValue& pageValue : pages) {
        const QJsonObject page = pageValue.toObject();
        const QSizeF pageSize = pageSizeOf(page);
        const int rawIndex = page.value(QStringLiteral("page_idx")).toInt(-1);
        if (rawIndex < 0) {
            result.document.warnings.append(QStringLiteral("MinerU 返回的页面缺少页码，已跳过"));
            continue;
        }
        const int pageNumber = options.normalizePageNumbers ? rawIndex + 1 : rawIndex;
        if (pageSize.isEmpty()) {
            // 没有页尺寸就无法归一化坐标。保留文字、放弃该页锚点，并显式告警：
            // 静默产出无锚点页会让题图裁切"看起来正常但永远为空"。
            result.document.warnings.append(
                QStringLiteral("MinerU 第 %1 页缺少页面尺寸，该页图片定位不可用").arg(pageNumber));
        }
        const PageText pageText = buildPageText(page, pageNumber, pageSize, &result.document);
        pageTexts.append(pageText.text);
        anyContent = anyContent || pageText.hasContent;
    }

    if (!anyContent) {
        result.error = QStringLiteral("MinerU 未能从该文档提取到任何文字");
        return result;
    }

    // 与本地 PdfExtractor 同构：换页符分隔，规则解析器据此生成 source.page。
    result.document.plainText = pageTexts.join(QChar(u'\f'));
    result.document.hasPageBoundaries = true;
    result.document.extractionBackend = result.backend.isEmpty()
        ? QStringLiteral("mineru")
        : QStringLiteral("mineru-%1").arg(result.backend);
    // MinerU 对扫描页会自行 OCR。标记来源便于复核页提示用户核对识别结果，
    // 与本地 Tesseract 路径保持同一语义。
    result.document.usedOcr = true;
    return result;
}

MineruAdaptResult adaptMineruDirectory(const QString& directory, const QString& sourcePath,
                                       const MineruParseOptions& options) {
    MineruAdaptResult result;
    result.document.sourcePath = sourcePath;

    const QString layoutPath = QDir(directory).filePath(QStringLiteral("layout.json"));
    QFile file(layoutPath);
    if (!file.exists()) {
        result.error = QStringLiteral("MinerU 结果目录缺少 layout.json");
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法读取 MinerU 版面数据：%1").arg(file.errorString());
        return result;
    }
    if (file.size() > kMaxLayoutJsonBytes) {
        result.error = QStringLiteral("MinerU 版面数据过大，已拒绝解析");
        return result;
    }
    return adaptMineruLayout(file.readAll(), sourcePath, options);
}

MineruAdaptResult adaptMineruZip(const QString& zipPath, const QString& sourcePath,
                                 const MineruParseOptions& options) {
    MineruAdaptResult result;
    result.document.sourcePath = sourcePath;

    QString error;
    const QByteArray layout = readLayoutJsonFromZip(zipPath, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    return adaptMineruLayout(layout, sourcePath, options);
}

} // namespace quizpane::studio
