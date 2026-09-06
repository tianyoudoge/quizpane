// ============================================================================
// 第三方原生 Provider 最小模板
// ============================================================================
//
// 这是一个能编译、能跑通 Host 全部调用流程的最小骨架，但业务数据是写死的
// （只有 1 道题、不真的登录、不真的发网络请求）。照抄这份文件到你自己的仓库后，
// 按文件里的 "TODO(你)" 注释逐个替换成你的真实实现。
//
// 完整的方法清单和每个字段的含义见同目录 README.md。ABI 契约本身
// （六个导出符号是什么、为什么用 C ABI 而不是 C++ 虚函数）见
// sdk/include/quizpane/provider_abi.h 的头部注释，那里的解释不在这里重复。
//
// 和 Java 开发经验的对照（帮助建立心智模型，不代表底层实现相同）：
//   - qp_provider_create/destroy   大致对应 Spring Bean 的构造/销毁
//   - qp_host_api_v1（Host 能力表）大致对应依赖注入进来的几个基础设施 Bean
//   - qp_provider_request 的 JSON  大致对应一个只有一个 Controller 方法、
//                                  用 method 字段做多路分发的 RPC 接口
//   - qp_provider_handle           大致对应这个 Bean 的实例状态（成员变量）
// ============================================================================

#include "quizpane/provider_abi.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// qp_provider_handle 是 Host 侧只认指针、不关心内部结构的不透明类型
// （C ABI 决定了它不能是 C++ 类，只能是 extern "C" 里声明的不完整类型）。
// 具体存什么字段完全由你决定：这里演示放一个"当前登录态"和"已保存的答案"。
struct qp_provider_handle {
    // Host 在 qp_provider_create 时传入的能力表，原样存一份留着后面用
    // （调用 host.log / host.secure_read / host.secure_write）。
    qp_host_api_v1 host{};

    // TODO(你)：换成真实的登录会话信息（token、cookie、用户 id 等）。
    bool loggedIn = false;

    // TODO(你)：换成真实的作答记录存储（比如按 attemptId 分组的 map）。
    // 这里只演示单次练习、单份作答的最简单情形。
    QJsonObject savedAnswers;
};

namespace {

// ----------------------------------------------------------------------
// JSON-RPC 风格的通用错误响应。id 原样回传，便于调用方（Host）匹配请求。
// ----------------------------------------------------------------------
QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message) {
    return {{"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

// ----------------------------------------------------------------------
// TODO(你)：这里演示"只有一道题的固定题库"。真实场景应该改成向你的后端
// 发起 HTTP 请求（或读取本地数据源），而不是像这样写死在代码里。
//
// 字段命名和结构必须严格遵守 Host 期望的协议（不能自己发明字段名），
// 具体字段含义参考 providers/demo/demo_provider.cpp 中同名字段的用法：
//   - options[].label：显示在选项前的字母（A/B/C/D）
//   - options[].contentHtml：选项正文，允许简单 HTML（用于公式/图片）
//   - correctChoice：正确选项在 options 数组中的下标（从 0 开始）
//   - solutionHtml：解析正文，同样允许简单 HTML
// ----------------------------------------------------------------------
QJsonObject templateQuestion(bool includeSolution) {
    QJsonObject question{
        {"id", "template-q1"},
        {"type", "single_choice"},
        {"contentHtml", "<p>这是模板题目，替换成你的真实题干。</p>"},
        {"difficulty", 1},
        {"options", QJsonArray{
            QJsonObject{{"index", 0}, {"label", "A"}, {"contentHtml", "选项 A"}},
            QJsonObject{{"index", 1}, {"label", "B"}, {"contentHtml", "选项 B"}},
        }},
    };
    if (includeSolution) {
        question.insert("correctChoice", 0);
        question.insert("solutionHtml", "<p>这里写解析正文。</p>");
    }
    return question;
}

// ----------------------------------------------------------------------
// 所有请求的统一入口。method 字段做多路分发，形态上等价于一个 Controller
// 类里塞了所有 @RequestMapping。题库变复杂后，建议按 method 前缀拆成
// 独立的处理函数（参考 sdk/src/declarative_provider.cpp 的重构建议：
// 用分发表代替一长串 if-else，见 spec/refactor/03-接口类题库实现.md）。
// ----------------------------------------------------------------------
QJsonObject handleRequest(qp_provider_handle* handle, const QJsonObject& request) {
    const auto id = request.value("id");
    const QString method = request.value("method").toString();

    // --- 生命周期：题库加载时调用一次 ---
    if (method == "provider.initialize") {
        // TODO(你)：如果需要登录，这里可以尝试用 host->secure_read 读取上次
        // 保存的 token，判断 sessionRestored 是否为 true。demo 模板演示的是
        // "完全不需要登录"的最简单情形。
        return {{"id", id}, {"result", QJsonObject{
            {"providerId", "com.example.template"},   // TODO(你)：换成你的 provider id
            {"providerVersion", "0.1.0"},
            {"providerAbi", 1},
            {"requiresLogin", false},                  // TODO(你)：需要登录则改 true
            {"sessionRestored", true},
        }}};
    }

    // --- 能力声明：Host 用它决定要不要显示登录入口、组卷数量选项等 UI ---
    if (method == "provider.capabilities") {
        return {{"id", id}, {"result", QJsonObject{
            {"loginMethods", QJsonArray{"none"}},  // TODO(你)：例如 QJsonArray{"password", "qrcode"}
            {"attempt", QJsonObject{
                {"management", "host_managed"},    // 练习进度由 Host 管理，Provider 只需诚实上报数据
                {"suggestedCounts", QJsonArray{5, 10, 15}},
                {"canResume", true},
            }},
        }}};
    }

    // --- 分类树：用户打开"选题库/分类"页面时调用 ---
    if (method == "catalog.list") {
        // TODO(你)：换成真实分类结构。availableQuestionCount 决定这个分类
        // 下能不能开始练习（canStartAttempt），必须和 attempt.questions 实际
        // 能返回的题量一致，否则用户会遇到"选了却抽不出题"的体验问题。
        return {{"id", id}, {"result", QJsonObject{{"nodes", QJsonArray{
            QJsonObject{
                {"id", "template-catalog"},
                {"title", "模板分类"},
                {"availableQuestionCount", 1},
                {"canStartAttempt", true},
            },
        }}}}};
    }

    // --- 开始一次新练习 ---
    if (method == "attempt.create") {
        // TODO(你)：真实场景下这里通常要向后端申请一个 attemptId，
        // 并清空/初始化这次练习的本地作答缓存。
        handle->savedAnswers = QJsonObject{};
        return {{"id", id}, {"result", QJsonObject{
            {"attemptId", "template-attempt-1"},
            {"title", "模板分类"},
            {"status", "active"},
            {"questionCount", 1},
            {"elapsedSeconds", 0},
        }}};
    }

    // --- 恢复一次未完成的练习（用户中途关闭软件后重新打开）---
    if (method == "attempt.get") {
        // TODO(你)：从持久化存储里查这个 attemptId 当前的状态。
        // 这里直接复用 attempt.create 的写死返回值做演示。
        return {{"id", id}, {"result", QJsonObject{
            {"attemptId", "template-attempt-1"},
            {"title", "模板分类"},
            {"status", "active"},
            {"questionCount", 1},
            {"elapsedSeconds", 0},
        }}};
    }

    // --- 拿到题目内容（不含答案）---
    if (method == "attempt.questions") {
        return {{"id", id}, {"result", QJsonObject{
            {"questions", QJsonArray{templateQuestion(/*includeSolution=*/false)}},
        }}};
    }

    // --- 用户作答/翻页时高频调用，负责把作答进度持久化 ---
    if (method == "attempt.saveAnswers") {
        // TODO(你)：这里的 params.answers 结构和 demo_provider 一致：
        // 每项包含 questionIndex 和 answer.choice（字符串形式的选项下标）。
        // 真实场景应该把它们写盘/发到后端，而不是像这样只存进内存成员。
        for (const auto& answerValue : request.value("params").toObject().value("answers").toArray()) {
            const auto answer = answerValue.toObject();
            handle->savedAnswers.insert(
                QString::number(answer.value("questionIndex").toInt()),
                answer.value("answer").toObject().value("choice"));
        }
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    }

    // --- 用户交卷 ---
    if (method == "attempt.submit") {
        // TODO(你)：如果统计/判分需要在服务端完成，这里应该真的发一次网络请求。
        return {{"id", id}, {"result", QJsonObject{{"ok", true}}}};
    }

    // --- 交卷后的统计结果 ---
    if (method == "attempt.report") {
        // TODO(你)：根据 handle->savedAnswers 与正确答案比对算出 correctCount。
        return {{"id", id}, {"result", QJsonObject{
            {"attemptId", "template-attempt-1"},
            {"title", "模板分类"},
            {"questionCount", 1},
            {"answerCount", handle->savedAnswers.size()},
            {"correctCount", 0},
            {"elapsedSeconds", 0},
        }}};
    }

    // --- 查看解析：这次带上 correctChoice/solutionHtml ---
    if (method == "attempt.solutions") {
        return {{"id", id}, {"result", QJsonObject{
            {"solutions", QJsonArray{templateQuestion(/*includeSolution=*/true)}},
        }}};
    }

    // 未识别的 method：JSON-RPC 惯例用 -32601 表示"方法不存在"。
    return errorResponse(id, -32601, QStringLiteral("方法不存在：%1").arg(method));
}

}  // namespace

// ============================================================================
// 以下六个函数是 C ABI 硬性要求导出的符号，函数名和签名不能改
// （Host 用 dlopen/LoadLibrary + 符号名精确匹配来找它们）。
// ============================================================================

// Host 加载动态库后第一个调用的函数，用于版本协商：如果返回值和 Host 期望的
// QP_PROVIDER_ABI_V1 不一致，Host 会拒绝加载，不会往下调用其他符号。
extern "C" QP_PROVIDER_EXPORT uint32_t qp_provider_abi_version() {
    return QP_PROVIDER_ABI_V1;
}

// 返回这个 Provider 的静态元信息（不需要实例即可查询，用于题库市场列表展示）。
// TODO(你)：id 要全局唯一（建议反向域名），version 随发布更新。
extern "C" QP_PROVIDER_EXPORT const char* qp_provider_descriptor_json() {
    return R"({"id":"com.example.template","name":"模板题库","version":"0.1.0","providerAbi":1})";
}

// 创建一个 Provider 实例。host 指针的生命周期由调用方保证，在
// qp_provider_destroy 之前始终有效，可以安全地把它整份拷贝存下来
// （见 qp_provider_handle::host）。
extern "C" QP_PROVIDER_EXPORT int qp_provider_create(
    const qp_host_api_v1* host, qp_provider_handle** out_handle) {
    // abi_version 不匹配时拒绝创建：防止新旧 Host/Provider 组合出现
    // 未定义行为（比如 Host 传入的能力表比 Provider 期望的字段更少）。
    if (!host || !out_handle || host->abi_version != QP_PROVIDER_ABI_V1) return 1;
    auto* handle = new qp_provider_handle;
    handle->host = *host;
    *out_handle = handle;
    return 0;
}

// 处理一次请求。注意：这里演示的是**同步**实现（收到请求就在本次调用栈里
// 直接算出结果并回调），仅适合完全离线、无网络 IO 的场景。
//
// TODO(你)：如果需要发起真实网络请求，qp_provider_request 应该立刻返回 0
// （表示"已受理"），网络请求发起后不要阻塞等待；等 HTTP 回调触发时再调用
// callback 把结果传回去。callback 可以来自任意线程，Host 侧会用信号槽的
// 队列连接转发回主线程，但你自己的 handle 内部状态（比如 savedAnswers）
// 如果会被 qp_provider_cancel/qp_provider_destroy 并发访问，需要自己加锁。
extern "C" QP_PROVIDER_EXPORT int qp_provider_request(
    qp_provider_handle* handle, const char* request_json, size_t request_size,
    qp_response_callback callback, void* user_data) {
    if (!handle || !request_json || !callback) return 1;

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        QByteArray(request_json, static_cast<qsizetype>(request_size)), &parseError);

    const QJsonObject response = document.isObject()
        ? handleRequest(handle, document.object())
        : errorResponse({}, -32700, parseError.errorString());

    const QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    callback(user_data, bytes.constData(), static_cast<size_t>(bytes.size()));
    return 0;
}

// 取消一个尚未完成的请求（比如用户离开页面时取消挂起的网络请求）。
// TODO(你)：本模板全同步、没有真正"挂起中"的请求，因此什么都不用做。
// 一旦你实现了异步网络请求，这里需要按 request_id 找到对应的
// QNetworkReply（或等价物）并 abort()。
extern "C" QP_PROVIDER_EXPORT int qp_provider_cancel(
    qp_provider_handle*, const char*, size_t) {
    return 0;
}

// 销毁实例，回收 qp_provider_create 分配的内存。
// TODO(你)：如果实现了异步请求，这里还需要确保所有挂起的回调都已经
// 取消或不会再被触发，避免回调触发时 handle 已经是悬空指针。
extern "C" QP_PROVIDER_EXPORT void qp_provider_destroy(qp_provider_handle* handle) {
    delete handle;
}
