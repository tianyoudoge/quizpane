# QuizPane Roadmap

## P0：首次公开发布

- [ ] 在 Intel macOS、Windows 10 和 Windows 11 完成干净系统验收；
- [ ] 在统信 UOS、银河麒麟 x86_64 完成构建与桌面集成验收；
- [ ] 完成 Provider Manifest v2、Ed25519 签名和信任管理；
- [ ] 配置 GitHub Actions 源码构建、测试、秘密扫描和依赖检查；
- [ ] 准备应用主界面、题库管理、答题解析截图；
- [ ] 由律师复核 PolyForm 非商业许可、商业授权和贡献者协议。

## P1：题库生态

- [ ] 完成 `.quizpane-bank` v1 数据模型；
- [ ] 题库制作器支持 CSV/JSON/QTI 导入；
- [ ] 完成 Zstandard 分块和图片按需加载；
- [ ] 完成 Ed25519 内容签名；
- [ ] 完成可选 XChaCha20-Poly1305 加密；
- [ ] 建立 Provider 权限、审核、投诉下架与签名吊销机制。

## P2：平台兼容

- [ ] Windows 7 / Qt 5.15 独立兼容分支；
- [ ] Linux ARM64 构建基线与 UOS/麒麟真机验证；
- [ ] 评估 LoongArch64 工具链和 Qt 6 可用性；
- [ ] Wayland 全局快捷键 portal/fallback 方案；
- [ ] macOS 10.14/10.15 Intel 兼容产物。

## 已完成

- [x] 无边框透明小窗、PIN、三档尺寸和紧凑滚动答题区；
- [x] 题库添加、切换、删除与拖入安装；
- [x] 老板键、托盘、macOS 菜单和关于页；
- [x] 草稿恢复与跨平台安全凭据接口；
- [x] C ABI + JSON Provider SDK、Demo Provider 和自动测试；
- [x] 题图近白背景透明化与内容裁剪；
- [x] 个人非商业免费、商业另行授权的源码可用许可。
