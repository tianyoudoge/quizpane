---
name: quizpane-spec-sync
description: Use for QuizPane implementation, bug fixes, refactors, behavior reviews, or test changes that require mapping product logic through spec/ before editing code and synchronizing affected spec documents afterward. Do not use for pure Git operations or unrelated prose-only work.
---

# QuizPane Spec Sync

Treat `spec/` as the maintained map from product behavior to code, not as a substitute for the live implementation. Code and executable tests remain authoritative when they conflict with stale prose or line numbers.

## Before editing code

1. Read `spec/README.md`, then read only the topic documents relevant to the requested change:
   - application/data-flow boundaries and bank contract: `spec/01-总览.md`
   - local extraction, PDF, OCR, anchors, and underlines: `spec/02-文档提取与扫描件处理.md`
   - rule parsing, answers, question types, review risk, and visual fallback: `spec/03-规则识别链路.md`
   - MinerU client, adapter, and cloud workflow: `spec/04-智能识别链路-MinerU.md`
   - review UI, manual edits, validation, packaging, and handoff: `spec/05-识别后手工编辑与导出.md`
   - known defects, gaps, and historical drift: `spec/todo.md`
2. Use the named functions, classes, data fields, and tests in the selected spec as search keys to locate the live code. Treat `file:line` references as navigation hints because line numbers drift.
3. Verify the relevant claim against its implementation, callers, validator/schema boundary, and existing tests before deciding what to change. If the spec is already wrong, identify that separately from the requested code change.

## While implementing

- Preserve the cross-layer constraints documented in `spec/README.md`, especially deterministic rule generation, explicit failure/review semantics, whole-bank answer policy, source privacy, and separation of formal assets from review-only assets.
- Keep behavior changes and their focused regression tests together. Use existing test targets named by the relevant spec before inventing a new harness.
- When current code and older `docs/` plans disagree, verify the live behavior and use `spec/` to record the current truth. Do not restore obsolete behavior merely to match a historical design document.

## Synchronize spec before finishing

Update the affected spec files in the same change whenever code changes any of these:

- user-visible workflow or editing capability;
- data structures, schema/validator contracts, or asset packaging;
- extraction/parsing rules, supported input shapes, or answer semantics;
- review risk levels, warnings, fallback, retry, recovery, or failure behavior;
- important code entry points, tests, known limitations, or completed TODO items.

Describe behavior and invariants first. Point to stable function/class names and repo-relative paths; add exact line numbers only when they materially speed up navigation. Update `spec/todo.md` when an issue is fixed, invalidated, newly discovered, or reprioritized. Do not claim broad verification that was not run.

Pure internal refactors that preserve every documented behavior may only need symbol/path corrections, but explicitly check that before omitting a spec edit.

## Verify the result

- Re-read the changed code, tests, and affected spec paragraphs as one contract.
- Check relative Markdown links under `spec/` and confirm newly cited repo paths/symbols exist.
- Build and run the smallest meaningful test set; for broad bank-studio/core changes, prefer the configured Release build and `ctest --preset release` when available.
- In the final handoff, state which spec files changed and report actual validation results, including any unverified areas.
