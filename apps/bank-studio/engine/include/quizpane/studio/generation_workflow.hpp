#pragma once

#include "quizpane/studio/review_result.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

namespace quizpane::studio {

enum class WorkflowStage {
    Idle,
    Extracting,
    Chunking,
    Done,
    Failed
};

struct WorkflowProgress {
    WorkflowStage stage = WorkflowStage::Idle;
    int completedSourceBlocks = 0;
    int totalSourceBlocks = 0;
    QString detail;
    int percent = -1;
    QString rulePass;
    QString documentName;
    QString sectionTitle;
    int sectionIndex = 0;
    int sectionCount = 0;
    int questionIndex = 0;
    int questionCount = 0;
    int processedQuestions = 0;
    int acceptedQuestions = 0;
    int reviewQuestions = 0;
};

using GeneratedBankCandidate = ReviewResult;

// 默认由程序根据解析到的答案证据决定整库策略；Included/None 只作为用户
// 明确覆盖。最终 bank.json 仍保持 included/none 两态，不引入混合题库。
enum class AnswerPolicyHint {
    Auto,
    Included,
    None
};

// 一组资料可由题目文件和独立答案/解析文件组成。离线路径分别提取两者，
// 题本分套后再按套题标题、题号和安全位置规则匹配；没有答案文件时 answerPath 为空。
struct SourceMaterialGroup {
    QString questionPath;
    QString answerPath;
    AnswerPolicyHint answerPolicy = AnswerPolicyHint::Auto;
    // 云解析结果 ZIP 的本地路径。非空时用 MinerU 的版面数据替代本机 PDF 文字层，
    // 其余流程（规则引擎、复核、打包）完全一致。
    // 上传与下载由 MineruExtractionJob 在本工作流之前完成——它是异步的，而这里
    // 的提取运行在工作线程里同步调用，不能塞进去。
    QString mineruZipPath;
    QString mineruAnswerZipPath;
};

// 规则结构化工作流：读取资料、跑规则引擎、发布候选 DTO。本身不发起网络请求；
// 需要云解析时由调用方先用 MineruExtractionJob 取得结果 ZIP，再经
// SourceMaterialGroup::mineruZipPath 传入。没有检查点/断点续传语义——规则引擎
// 是纯函数，相同输入总产生相同输出，重跑一次的成本远低于维护一套跨进程续传
// 状态机。
class GenerationWorkflow final : public QObject {
    Q_OBJECT
  public:
    explicit GenerationWorkflow(QObject* parent = nullptr);

    // 完全离线的规则结构化入口：读取文档并跑规则引擎。相同输入始终产生相同
    // 输出，适合规整题库快速导入。活跃任务上的重复调用会被忽略。
    void startRuleBased(const QStringList& sourcePaths);
    void startRuleBased(const QList<SourceMaterialGroup>& sources);

    // 取消当前后台任务；调用后结果会被丢弃，不会发出 questionsReady/finished。
    void cancel();

    // 返回后台规则解析任务是否仍在运行。
    bool isActive() const {
        return active_;
    }

  signals:
    void progressChanged(const quizpane::studio::WorkflowProgress& progress);
    void questionsReady(const quizpane::studio::GeneratedBankCandidate& candidate);
    void failed(const QString& error);
    void finished();

  private:
    // 发布不持久化的进度快照。
    void publish(WorkflowStage stage, const QString& detail);

    bool active_ = false;
};

} // namespace quizpane::studio

Q_DECLARE_METATYPE(quizpane::studio::WorkflowProgress)
Q_DECLARE_METATYPE(quizpane::studio::GeneratedBankCandidate)
Q_DECLARE_METATYPE(quizpane::studio::AnswerPolicyHint)
