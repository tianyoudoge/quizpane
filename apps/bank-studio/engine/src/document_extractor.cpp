#include "quizpane/studio/document_extractor.hpp"

#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QStringDecoder>
#include <QTextCodec>

namespace quizpane::studio {
namespace {

bool hasSuffix(const QString& path, const QStringList& suffixes) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffixes.contains(suffix);
}

// UTF-8 严格解码失败（出现非法字节序列）时回退到 GB18030，覆盖国内用户
// 常见的 Windows 记事本"ANSI"编码保存的 TXT 文件。BOM 由 QStringDecoder
// 自动识别并跳过。GB18030 不在 QStringConverter 内置编码里，需要
// Qt6::Core5Compat 提供的 QTextCodec。
QString decodeText(const QByteArray& bytes, QString* error) {
    QStringDecoder utf8Decoder(QStringConverter::Utf8);
    const QString utf8Text = utf8Decoder.decode(bytes);
    if (!utf8Decoder.hasError()) return utf8Text;

    QTextCodec* gbCodec = QTextCodec::codecForName("GB18030");
    if (!gbCodec) {
        *error = QStringLiteral("文件编码既不是有效 UTF-8，也无法用 GB18030 解码");
        return {};
    }
    QTextCodec::ConverterState state;
    const QString gbText = gbCodec->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars > 0) {
        *error = QStringLiteral("无法识别文件编码，请转存为 UTF-8 后重试");
        return {};
    }
    return gbText;
}

}  // namespace

bool TxtMarkdownExtractor::supports(const QString& path) const {
    return hasSuffix(path, {"txt", "md", "markdown"});
}

ExtractedDocument TxtMarkdownExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return result;
    }
    const QByteArray bytes = file.readAll();
    QString error;
    const QString text = decodeText(bytes, &error);
    if (!error.isEmpty()) {
        result.error = error;
        return result;
    }
    if (text.trimmed().isEmpty()) {
        result.error = QStringLiteral("文件内容为空");
        return result;
    }
    result.plainText = text;
    return result;
}

bool DocxExtractor::supports(const QString& path) const { return hasSuffix(path, {"docx"}); }

ExtractedDocument DocxExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    result.error = QStringLiteral("暂不支持 DOCX 文档提取，敬请期待后续版本");
    return result;
}

bool PdfExtractor::supports(const QString& path) const { return hasSuffix(path, {"pdf"}); }

ExtractedDocument PdfExtractor::extract(const QString& path) const {
    ExtractedDocument result;
    result.sourcePath = path;
    result.error = QStringLiteral("暂不支持 PDF 文档提取，敬请期待后续版本");
    return result;
}

ExtractorRegistry::ExtractorRegistry() = default;

ExtractedDocument ExtractorRegistry::extract(const QString& path) const {
    if (txtMarkdown_.supports(path)) return txtMarkdown_.extract(path);
    if (docx_.supports(path)) return docx_.extract(path);
    if (pdf_.supports(path)) return pdf_.extract(path);
    ExtractedDocument result;
    result.sourcePath = path;
    result.error = QStringLiteral("不支持的文件格式：%1").arg(QFileInfo(path).suffix());
    return result;
}

}  // namespace quizpane::studio
