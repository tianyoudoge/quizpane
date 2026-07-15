#pragma once

#include "quizpane/draft_store.hpp"
#include "quizpane/provider_installer.hpp"
#include "quizpane/provider_loader.hpp"

namespace quizpane {

// UI 的应用服务入口。MainWindow 不再决定服务的构造组合；测试可单独替换这层，
// 后续加入统计或同步服务也不需要继续扩大窗口的持久化职责。
class AppServices final {
public:
    ProviderLoader& provider() { return provider_; }
    ProviderInstaller& installer() { return installer_; }
    DraftStore& drafts() { return drafts_; }

private:
    ProviderLoader provider_;
    ProviderInstaller installer_;
    DraftStore drafts_;
};

}  // namespace quizpane
