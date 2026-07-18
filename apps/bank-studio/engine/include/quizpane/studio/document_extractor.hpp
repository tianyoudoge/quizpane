#pragma once

#include <QString>
#include <QStringList>
#include <QHash>
#include <QList>
#include <QPair>
#include <QRectF>

namespace quizpane::studio {

// PDF 文字层中的题号/选项标签坐标。坐标已经归一化到 0..1，因而规则生成器可
// 以同一套逻辑裁切任意渲染分辨率的页面，而不必 OCR 图片里的数学符号。
struct PdfTextAnchor {
    QString text;
    QRectF bounds;
};

// 原卷中确实检测到的下划线字符范围。QPdfDocument 不公开“文字带下划线”样式，
// 因而该信息来自文字选择框与渲染页中水平细线的几何交叉检测；没有足够证据时
// 不填充，绝不根据题目选项猜测。
struct PdfUnderlineDecoration {
    QString text;
    QList<QPair<int, int>> ranges;
    QRectF bounds;
};

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
    // 文字 PDF 也可能把统计图、图形推理题嵌为位图。扫描 PDF 会在提取时保留
    // 渲染页；文字 PDF 的页面则由规则生成器只在确认需要原卷视觉上下文时按需
    // 载入，避免“全卷每页渲染 + PNG 压缩”拖慢普通纯文字题库。
    QHash<int, QByteArray> pageImages;
    // 对文字型 PDF，保留题号与 A/B/C/D 标签的版面位置。它只用于“文字层没有
    // 选项内容、但选项本身是图或公式”的安全小图裁切。
    QHash<int, QList<PdfTextAnchor>> questionAnchors;
    QHash<int, QList<PdfTextAnchor>> optionLabelAnchors;
    // 每页文字行的坐标。规则生成器用它把阅读材料裁成原卷片段，保留 PDF
    // 文字层表达不了的下划线、填空横线和嵌入式图片横线。
    QHash<int, QList<PdfTextAnchor>> lineAnchors;
    QHash<int, QList<PdfUnderlineDecoration>> underlineDecorations;
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

// 精确检测 PDF 中真正绘制出来的下划线。这个步骤刻意不在 extract() 中执行：
// 规则生成器先确认某份共享材料确实被“划线/横线/标注/标记”类小题引用，再把那
// 几行材料传进来，避免整卷逐字符扫图造成延迟和误召。
void detectPdfUnderlinesForCandidateLines(
    ExtractedDocument* document, const QHash<int, QStringList>& candidateLinesByPage);

// 为已经确认需要原卷视觉上下文的页面补齐本地 PNG 缓存。仅对 PDF 生效；传入
// 的页码从 1 开始。重复请求会命中已有缓存，不会再次渲染。
void ensurePdfPageImages(ExtractedDocument* document, const QList<int>& pageNumbers);

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
