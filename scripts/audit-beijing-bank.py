#!/usr/bin/env python3
"""Compare the 2011 Beijing PDF ground truth with a generated QuizPane bank."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


QUESTION_COUNT = 135
MATERIAL_GROUPS = [
    range(36, 41), range(41, 46), range(46, 51), range(51, 56),
    range(56, 61), range(61, 66), range(66, 71), range(116, 121),
    range(121, 126), range(126, 131), range(131, 136),
]


def numbered_blocks(text: str) -> dict[int, dict[str, object]]:
    pages = text.split("\f")
    anchors: list[tuple[int, int, int]] = []
    offset = 0
    expected = 1
    marker = re.compile(r"(?m)^[ \t]*(\d{1,3})[ \t]*[、.．]")
    for page_number, page in enumerate(pages, 1):
        for match in marker.finditer(page):
            number = int(match.group(1))
            if number != expected:
                continue
            anchors.append((number, offset + match.start(), page_number))
            expected += 1
            if expected > QUESTION_COUNT:
                break
        offset += len(page) + 1
    if expected <= QUESTION_COUNT:
        raise RuntimeError(f"PDF numbering stopped at {expected - 1}, expected {QUESTION_COUNT}")

    joined = "\f".join(pages)
    result: dict[int, dict[str, object]] = {}
    for index, (number, start, page_number) in enumerate(anchors):
        end = anchors[index + 1][1] if index + 1 < len(anchors) else len(joined)
        block = re.sub(r"[ \t]+", " ", joined[start:end]).strip()
        result[number] = {"page": page_number, "text": block}
    return result


def answer_for(block: str) -> str:
    patterns = [
        r"(?:故)?正确答案(?:为|是)\s*([A-D](?:\s*[,，、;；/|+]?\s*[A-D]){0,3})",
        r"答案(?:为|是|：|:)\s*([A-D](?:\s*[,，、;；/|+]?\s*[A-D]){0,3})",
    ]
    for pattern in patterns:
        match = re.search(pattern, block, re.I)
        if match:
            return "".join(sorted(set(re.findall(r"[A-D]", match.group(1).upper()))))
    return ""


def original_number(question: dict[str, object]) -> int | None:
    match = re.search(r"-q(\d+)-", str(question.get("id", "")))
    return int(match.group(1)) if match else None


def option_letters(question: dict[str, object]) -> str:
    answer = question.get("answer", {})
    if not isinstance(answer, dict):
        return ""
    ids = answer.get("optionIds", [])
    return "".join(sorted(str(value).upper() for value in ids))


def audit(bank: dict[str, object], baseline: dict[int, dict[str, object]]) -> list[dict[str, object]]:
    questions = bank.get("questions", [])
    generated = {
        number: question
        for question in questions
        if isinstance(question, dict) and (number := original_number(question)) is not None
    }
    issues: list[dict[str, object]] = []
    for number in range(1, QUESTION_COUNT + 1):
        question = generated.get(number)
        if question is None:
            issues.append({"question": number, "kind": "missing-question", "detail": "题目未生成"})
            continue
        options = question.get("options", [])
        if not isinstance(options, list) or len(options) != 4:
            issues.append({"question": number, "kind": "option-count", "detail": f"生成选项数={len(options) if isinstance(options, list) else 'invalid'}"})
        source = question.get("source", {})
        source_page = source.get("page") if isinstance(source, dict) else None
        expected_page = baseline[number]["page"]
        if source_page != expected_page:
            issues.append({"question": number, "kind": "source-page", "detail": f"PDF 第 {expected_page} 页，题库标记第 {source_page} 页"})
        stem = str(question.get("stem", ""))
        if re.search(r"\n\s*(?:[一二三四五六七八九十]+、|\d{1,3}[、.．])", stem):
            issues.append({"question": number, "kind": "stem-contamination", "detail": "题干混入后续题号或章节标题"})
        expected_answer = str(baseline[number].get("answer", ""))
        actual_answer = option_letters(question)
        if expected_answer and actual_answer and expected_answer != actual_answer:
            issues.append({"question": number, "kind": "answer-mismatch", "detail": f"PDF={expected_answer} 题库={actual_answer}"})

    for group_index, group in enumerate(MATERIAL_GROUPS, 1):
        ids = {
            str(generated[number].get("materialId", ""))
            for number in group if number in generated
        }
        if ids != {next(iter(ids), "")} or "" in ids:
            issues.append({"question": f"{group.start}-{group.stop - 1}", "kind": "material-link", "detail": f"资料组 {group_index} materialId={sorted(ids)}"})
    for left, right in zip(MATERIAL_GROUPS, MATERIAL_GROUPS[1:]):
        left_ids = {str(generated[n].get("materialId", "")) for n in left if n in generated}
        right_ids = {str(generated[n].get("materialId", "")) for n in right if n in generated}
        if left_ids and left_ids == right_ids:
            issues.append({"question": f"{left.start}-{right.stop - 1}", "kind": "material-merged", "detail": "两个独立资料组被合并"})
    return issues


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--questions", type=Path, required=True)
    parser.add_argument("--answers", type=Path, required=True)
    parser.add_argument("--bank", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    question_blocks = numbered_blocks(args.questions.read_text(encoding="utf-8"))
    answer_blocks = numbered_blocks(args.answers.read_text(encoding="utf-8"))
    baseline: dict[int, dict[str, object]] = {}
    for number in range(1, QUESTION_COUNT + 1):
        baseline[number] = {
            **question_blocks[number],
            "answer": answer_for(str(answer_blocks[number]["text"])),
            "answerPage": answer_blocks[number]["page"],
        }

    bank = json.loads(args.bank.read_text(encoding="utf-8"))
    issues = audit(bank, baseline)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "baseline.json").write_text(
        json.dumps(baseline, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.output_dir / "issues.json").write_text(
        json.dumps(issues, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = ["# 2011 北京卷 PDF 基线", ""]
    for number in range(1, QUESTION_COUNT + 1):
        item = baseline[number]
        lines.extend([
            f"## 第 {number} 题",
            f"- 题目 PDF 页：{item['page']}",
            f"- 答案 PDF 页：{item['answerPage']}",
            f"- 标准答案：{item['answer'] or '未自动提取'}",
            "",
            "```text",
            str(item["text"]),
            "```",
            "",
        ])
    (args.output_dir / "baseline.md").write_text("\n".join(lines), encoding="utf-8")

    print(json.dumps({
        "baselineQuestions": len(baseline),
        "generatedQuestions": len(bank.get("questions", [])),
        "issues": len(issues),
        "issueKinds": sorted({str(issue["kind"]) for issue in issues}),
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
