#pragma once

#include <QHash>
#include <QString>

namespace quizpane {

// AI explanations belong to the installed declarative bank, separately from its
// official answer/solution fields. Native providers have no writable bank.json.
QHash<QString, QString> loadAiExplanations(const QString& bankPath);
bool saveAiExplanation(const QString& bankPath, const QString& questionId,
                       const QString& html, QString* error = nullptr);

}  // namespace quizpane
