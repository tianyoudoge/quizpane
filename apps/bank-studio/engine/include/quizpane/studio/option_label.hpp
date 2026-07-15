#pragma once

#include <QRegularExpression>
#include <QString>

namespace quizpane::studio {

QString canonicalOptionLabel(const QString& raw);
const QRegularExpression& optionLinePattern();

}  // namespace quizpane::studio
