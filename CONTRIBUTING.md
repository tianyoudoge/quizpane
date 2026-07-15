# Contributing / 参与贡献

感谢你帮助 QuizPane 适配更多桌面平台。提交代码前请阅读 [`CLA.md`](CLA.md)，并在 Pull Request 描述中确认 `I have read and agree to the QuizPane CLA.`。这使维护者能够同时维护个人非商业免费版和商业授权版。

## 开发流程

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

请从功能分支提交 Pull Request，不要提交 `build/`、`dist/`、动态库、安装包、凭据、抓包、真实题库或第三方平台私有协议。

代码注释以中文为主，标准 API、协议名和无法准确翻译的术语保留英文。注释重点解释
约束、边界和“为什么”，不使用 Java、jQuery、Maven 等跨技术栈类比重复解释 Qt/C++
基础语义。面向用户的按钮文案必须显式设置，不能依赖平台默认的 Yes/No 翻译。

## 帮助构建其他平台

我们尤其欢迎 Windows 10/11、Intel macOS、统信 UOS 和银河麒麟用户提供可复现的构建报告。请在目标系统的干净环境中：

1. 记录操作系统版本、CPU 架构、编译器、Qt 和 glibc（Linux）版本；
2. 完成 Release 构建并运行全部测试；
3. 验证启动、透明窗口、托盘、老板键、安全凭据和 Demo 题库；
4. 生成 SHA-256；
5. 提交 PR 更新构建文档，并在 Issue 中附构建日志、测试结果和校验值。

为了供应链安全，维护者不会直接发布来源不明的二进制。社区产物需提供对应 commit、完整构建命令和可复现记录，经维护者复核后才会进入正式 Release。

## English

Thank you for helping QuizPane support more desktop platforms. By contributing, you confirm that you have the right to submit the work and agree that maintainers may distribute it under both the PolyForm Noncommercial License 1.0.0 and separate commercial licenses. A CLA may be required for substantial contributions.

Please build and test on the target platform, report the exact OS/architecture/toolchain/Qt versions, attach reproducible commands and checksums, and never commit credentials, captures, proprietary question content, private protocols, or generated binaries. See the Chinese section above for the release-artifact review process.
