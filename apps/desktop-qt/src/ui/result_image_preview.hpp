#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class QImage;
class QLabel;

namespace quizpane::ui {

struct CompactResultRow {
    QString rangeLabel;
    QString answerSequence;
};

// 结果图按预览窗口设计，避免生成 1080px 宽图后必须横向滚动。
int resultImagePixelWidth();

// 无答案题库不需要逐题判分卡片。按固定题数合并选择，既保留题序，也能
// 让长题库的结果图保持紧凑（例如：1–5题  AABBC）。
QList<CompactResultRow> compactResultRows(const QStringList& answerTokens,
                                          int groupSize = 5);

// QScrollArea 在 setWidget() 时会按空 QLabel 的尺寸固定内容区域。设置结果长图
// 后必须同步刷新 QLabel 尺寸，否则滚动区只会露出长图左上角的一小块。
void setResultPreviewImage(QLabel* preview, const QImage& image);

}  // namespace quizpane::ui
