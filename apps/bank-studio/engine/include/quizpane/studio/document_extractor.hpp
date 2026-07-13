#pragma once

#include <QString>

namespace quizpane::studio {

// 单个文件的提取结果。error 非空表示提取失败或该格式尚不支持，
// plainText 此时应为空，调用方据此把该文件标记为跳过，而不是把
// 空字符串误当成"文档确实没有内容"。
struct ExtractedDocument {
    QString sourcePath;
    QString plainText;
    QString error;
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

// DOCX/PDF 提取尚未实现（需要引入 OOXML/PDF 解析依赖后再补上）。
// 这两个类只占位，保证 ExtractorRegistry 能识别对应扩展名并给出明确的
// "暂不支持"提示，而不是静默失败或被误判成未知格式。
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

}  // namespace quizpane::studio
