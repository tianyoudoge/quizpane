#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QSet>

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
    QString assetUrl(const QJsonObject& asset) const;
    // 兼容早期制作器导出的“单 catalog + source.sectionId”多套题库：加载后
    // 仅在内存中展开成每套一个 catalog，原 bank.json 无需迁移。
    void expandSectionCatalogs();
    // 按 questions 数组中的相对顺序把某分类下的题目分组：普通题是只含一道题的
    // 单元，共享同一 materialId 的题目合并成一个不可拆分的题组单元，组内顺序
    // 和组出现的位置都取首次出现的位置，不做任何重排。
    QVector<QJsonArray> buildUnits(const QString& catalogId) const;
    // 一次性枚举 practice/history/<providerId>/* 载入内存表（questionId ->
    // "correct"/"wrong"），供 catalog.list 的掌握度统计和 attempt.create 的
    // 掌握题过滤共用，避免逐题各发一次 QSettings::value()。
    QHash<QString, QString> practiceHistoryByQuestion() const;
    // attempt.report 判分和 attempt.solutions 都需要“带答案”的题目视图；同一个
    // attempt 内两者常常先后被请求，这里缓存一份，避免重复跑一遍 hostQuestions(true)
    // 的 HTML 转换和答案匹配。attempt.create 换卷时失效。
    QJsonArray solutionsView() const;

    // 9 个 RPC 方法各自的处理体，从原来 request() 里的一长串 if 链拆出来，
    // 让每个方法体成为独立可读、可单测的小函数；request() 只做分发。
    QJsonObject handleInitialize(const QJsonValue& id) const;
    QJsonObject handleCapabilities(const QJsonValue& id) const;
    QJsonObject handleCatalogList(const QJsonValue& id) const;
    QJsonObject handleAttemptCreate(const QJsonValue& id, const QJsonObject& params);
    QJsonObject handleAttemptQuestions(const QJsonValue& id) const;
    QJsonObject handleSaveAnswers(const QJsonValue& id, const QJsonObject& params);
    QJsonObject handleSubmit(const QJsonValue& id) const;
    QJsonObject handleReport(const QJsonValue& id) const;
    QJsonObject handleSolutions(const QJsonValue& id) const;

    QString providerId_, providerName_, providerVersion_, bankTitle_, activeCatalogTitle_;
    QJsonArray catalogs_, questions_, materials_, activeQuestions_;
    QHash<QString, QJsonObject> materialsById_;
    // catalogId -> 该分类下题目数。load 时预算一次，catalog.list 不必再对每个
    // 分类线性遍历全部题目（题库大、分类多时 O(题×分类) 会卡目录加载）。
    QHash<QString, int> questionCountByCatalog_;
    QString bankDirectory_;
    QHash<int, QSet<int>> answers_;
    bool hasAnswerKey_ = true;
    // solutionsView() 的缓存；mutable 是因为该视图只依赖 activeQuestions_/
    // answers_ 等已有状态，对外仍是只读查询，不应强迫调用方拿非 const 引用。
    mutable QJsonArray solutionsCache_;
    mutable bool solutionsCacheValid_ = false;
};

}  // namespace quizpane
