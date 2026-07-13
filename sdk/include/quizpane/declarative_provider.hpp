#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace quizpane {

class DeclarativeProvider final {
public:
    bool load(const QString& bankPath, QString* error = nullptr);
    [[nodiscard]] QJsonObject descriptor() const;
    [[nodiscard]] QJsonObject request(const QJsonObject& request);
    [[nodiscard]] bool isLoaded() const { return !providerId_.isEmpty(); }
    void unload();

private:
    QJsonObject error(const QJsonValue& id, const QString& message) const;
    QJsonArray hostQuestions(bool includeSolutions) const;
    QJsonObject findCatalog(const QString& id) const;
    QString providerId_, providerName_, providerVersion_, bankTitle_, activeCatalogTitle_;
    QJsonArray catalogs_, questions_, activeQuestions_;
    QHash<int, int> answers_;
};

}  // namespace quizpane
