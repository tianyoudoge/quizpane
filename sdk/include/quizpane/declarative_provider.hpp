#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

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
    QJsonArray hostMaterials() const;
    QJsonObject findCatalog(const QString& id) const;
    // 按 questions 数组中的相对顺序把某分类下的题目分组：普通题是只含一道题的
    // 单元，共享同一 materialId 的题目合并成一个不可拆分的题组单元，组内顺序
    // 和组出现的位置都取首次出现的位置，不做任何重排。
    QVector<QJsonArray> buildUnits(const QString& catalogId) const;

    QString providerId_, providerName_, providerVersion_, bankTitle_, activeCatalogTitle_;
    QJsonArray catalogs_, questions_, materials_, activeQuestions_;
    QHash<QString, QJsonObject> materialsById_;
    // catalogId -> 该分类下题目数。load 时预算一次，catalog.list 不必再对每个
    // 分类线性遍历全部题目（题库大、分类多时 O(题×分类) 会卡目录加载）。
    QHash<QString, int> questionCountByCatalog_;
    QHash<int, int> answers_;
};

}  // namespace quizpane
