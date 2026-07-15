#pragma once

#include <QString>
#include <QStringList>

namespace quizpane::studio {

// 单个文件的提取结果。error 非空表示提取失败或该格式尚不支持，
// plainText 此时应为空，调用方据此把该文件标记为跳过，而不是把
// 空字符串误当成"文档确实没有内容"。
struct ExtractedDocument {
    QString sourcePath;
    QString plainText;
    QString error;
    // 可恢复的逐页问题，例如某一页 OCR 失败。调用方应提示用户，但仍可继续
    // 使用其余成功页面；error 只表示整份文档无法使用。
    QStringList warnings;
    // 文本由 PDF/OCR 按页提取时，使用换页符分隔。规则解析器据此生成 source.page，
    // TXT/DOCX 没有稳定页码时保持 false，避免伪造来源位置。
    bool hasPageBoundaries = false;
    // 扫描页经过本地 OCR 时置 true，仅用于提示识别来源，不改变后续解析路径。
    bool usedOcr = false;
};

// 单一文档格式的提取器。supports() 只看扩展名，不打开文件，方便
// ExtractorRegistry 在选择具体实现前先做一次快速分派。
class DocumentExtractor {
  public:
    virtual ~DocumentExtractor() = default;
    virtual bool supports(const QString& path) const = 0;
    virtual ExtractedDocument extract(const QString& path) const = 0;
};

// TXT/Markdown：本地直接读取，自动识别 UTF-8/UTF-8 BOM/GB18030 编码。
class TxtMarkdownExtractor final : public DocumentExtractor {
  public:
    bool supports(const QString& path) const override;
    ExtractedDocument extract(const QString& path) const override;
};

// DOCX 直接读取 OOXML ZIP 中的 document.xml，不依赖 Office 或脚本运行时。
class DocxExtractor final : public DocumentExtractor {
  public:
    bool supports(const QString& path) const override;
    ExtractedDocument extract(const QString& path) const override;
};

class PdfExtractor final : public DocumentExtractor {
  public:
    bool supports(const QString& path) const override;
    ExtractedDocument extract(const QString& path) const override;
};

// 制作器唯一需要持有的入口：按扩展名找到合适的 DocumentExtractor 并提取。
// 找不到匹配格式时返回的 error 会提示具体扩展名，方便用户理解为什么某个
// 文件被跳过。
class ExtractorRegistry final {
  public:
    ExtractorRegistry();
    ExtractedDocument extract(const QString& path) const;

  private:
    TxtMarkdownExtractor txtMarkdown_;
    DocxExtractor docx_;
    PdfExtractor pdf_;
};

} // namespace quizpane::studio
