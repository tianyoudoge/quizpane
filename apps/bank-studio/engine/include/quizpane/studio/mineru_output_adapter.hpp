#pragma once

#include "quizpane/studio/document_extractor.hpp"

#include <QByteArray>
#include <QHash>
#include <QString>

namespace quizpane::studio {

// MinerU 云解析结果适配器。
//
// 定位：MinerU 只替换“文档感知”这一层，不理解真题语义。适配器把 MinerU 的
// 版面 JSON 翻译成与本地 PdfExtractor 完全同构的 ExtractedDocument，因而下游
// 规则引擎、复核页和打包校验都无需知道解析来自云端还是本地。
//
// 为什么以 middle/layout JSON 为主入口而不是 content_list.json：
//   - middle/layout JSON 提供 span 级 bbox；content_list.json 只有段落级 bbox。
//     真题里 “A. 甲  B. 乙” 常被排在同一行，段落级坐标无法区分四个选项标签，
//     而 optionLabelAnchors 恰恰要求每个标签有独立坐标（图片/公式选项的裁切
//     完全依赖它）。
//   - layout.json 的 discarded_blocks 已把页眉、页脚、页码从正文中分离出来，
//     这正是 Day11 第 130 题“页脚粘进选项行”的确定性解法：页眉页脚在结构上
//     就不在 para_blocks 里，无需靠文案黑名单猜测。
// content_list.json 仅用于诊断与人工比对，不参与锚点构建。
struct MineruParseOptions {
    // MinerU 页码从 0 开始，ExtractedDocument 统一 1-based。
    // 该开关仅供夹具测试构造异常输入时使用，正式路径恒为 true。
    bool normalizePageNumbers = true;
    // 仅表示本次请求是否显式强制 OCR，不能用“结果来自 MinerU”代替。
    // MinerU 的 VLM/自动检测并不等同于用户强制扫描件识别。
    bool usedOcr = false;
};

// 解析结果。error 非空表示这份 MinerU 输出无法使用；此时不应回退到“空文档”
// 继续生成，而应让调用方显式失败或改走本地解析。
struct MineruAdaptResult {
    ExtractedDocument document;
    QString error;
    // MinerU 模型版本（layout.json 的 _backend/_version_name）。写入诊断元数据，
    // 使云端模型静默升级后仍能解释回归变化。
    QString backend;
    QString versionName;
};

// 把 MinerU middle/layout JSON 的字节内容适配成 ExtractedDocument。
// sourcePath 用于填充 ExtractedDocument::sourcePath（诊断与资产命名需要），
// 传入原始 PDF 路径而不是 ZIP 路径：最终裁图仍以原卷像素为准。
MineruAdaptResult adaptMineruLayout(const QByteArray& layoutJson, const QString& sourcePath,
                                    const MineruParseOptions& options = {});

// 从已解压的 MinerU 结果目录构建 ExtractedDocument。
// 目录需包含 layout.json 或 *_middle.json；images/ 存在时按需读取（当前仅记录，不预载）。
MineruAdaptResult adaptMineruDirectory(const QString& directory, const QString& sourcePath,
                                       const MineruParseOptions& options = {});

// 从 MinerU 下载的 ZIP 构建 ExtractedDocument。
// ZIP 条目会经过路径校验（拒绝绝对路径、`..` 穿越）与体积上限检查，
// 避免 ZIP Slip 与压缩炸弹；这些输入来自网络，必须按不可信数据处理。
MineruAdaptResult adaptMineruZip(const QString& zipPath, const QString& sourcePath,
                                 const MineruParseOptions& options = {});

} // namespace quizpane::studio
