#pragma once

namespace quizpane {

// 窗口整体尺寸偏好，登录页、目录页、做题页、解析页和课程伴侣页共用。
// 从 MainWindow 提取到独立头文件，使三个页面控制器无需持有 MainWindow*
// 或访问其私有成员即可读取当前尺寸偏好。
enum class UiSize { Small, Medium, Large };

}  // namespace quizpane
