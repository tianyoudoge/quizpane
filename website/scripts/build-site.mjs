#!/usr/bin/env node
// Zero-dependency static site build: copies website/src to website/dist, fingerprints
// every static asset plus the browser-loaded CSS/JS, and rewrites references. HTML
// and content.json keep stable names so they can be revalidated on every visit.

import { createHash } from "node:crypto";
import { mkdir, rm, readFile, writeFile, cp, readdir, stat, rename } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const websiteDir = path.resolve(__dirname, "..");
const srcDir = path.join(websiteDir, "src");
const distDir = path.join(websiteDir, "dist");

function hashOf(buffer) {
  return createHash("sha256").update(buffer).digest("hex").slice(0, 10);
}

async function fileExists(p) {
  try {
    await stat(p);
    return true;
  } catch {
    return false;
  }
}

async function walkFiles(dir) {
  const files = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const entryPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...(await walkFiles(entryPath)));
    } else if (entry.isFile()) {
      files.push(entryPath);
    }
  }
  return files;
}

function posixRelative(from, to) {
  return path.relative(from, to).split(path.sep).join("/");
}

async function fingerprintFile(filePath) {
  const contents = await readFile(filePath);
  const ext = path.extname(filePath);
  const base = path.basename(filePath, ext);
  const fingerprinted = path.join(path.dirname(filePath), `${base}.${hashOf(contents)}${ext}`);
  await rename(filePath, fingerprinted);
  return fingerprinted;
}

async function rewriteTextReferences(distDir, replacements) {
  const textExtensions = new Set([".html", ".js", ".css", ".json"]);
  const files = await walkFiles(distDir);
  for (const filePath of files) {
    if (!textExtensions.has(path.extname(filePath))) continue;
    let text = await readFile(filePath, "utf8");
    for (const [original, fingerprinted] of replacements) {
      text = text.split(original).join(fingerprinted);
    }
    await writeFile(filePath, text, "utf8");
  }
}

async function build() {
  if (!(await fileExists(srcDir))) {
    throw new Error(`Missing source directory: ${srcDir}`);
  }

  await rm(distDir, { recursive: true, force: true });
  await mkdir(distDir, { recursive: true });
  await cp(srcDir, distDir, { recursive: true });

  // Assets are the only paths served with `immutable`; fingerprint every file
  // beneath that directory before rewriting HTML, JS and content.json links.
  const assetDir = path.join(distDir, "assets");
  const assetReferences = [];
  for (const assetPath of await walkFiles(assetDir)) {
    const original = posixRelative(distDir, assetPath);
    const fingerprintedPath = await fingerprintFile(assetPath);
    assetReferences.push([original, posixRelative(distDir, fingerprintedPath)]);
  }
  await rewriteTextReferences(distDir, assetReferences);

  // CSS and JS are content-addressed too, and live under /assets/ so the
  // existing immutable Nginx rule applies to them on every page.
  const runtimeReferences = [];
  const generatedDir = path.join(assetDir, "generated");
  await mkdir(generatedDir, { recursive: true });
  for (const name of ["main.css", "main.js", "course-companion.js"]) {
    const sourcePath = path.join(distDir, name);
    const contents = await readFile(sourcePath);
    const ext = path.extname(name);
    const base = path.basename(name, ext);
    const fingerprintedName = `${base}.${hashOf(contents)}${ext}`;
    const destination = path.join(generatedDir, fingerprintedName);
    await rename(sourcePath, destination);
    runtimeReferences.push([name, posixRelative(distDir, destination)]);
  }
  await rewriteTextReferences(distDir, runtimeReferences);

  const fileCount = await countFiles(distDir);
  console.log(`Built website/dist (${fileCount} files).`);
  for (const [original, fingerprinted] of runtimeReferences) {
    console.log(`  ${original} -> ${fingerprinted}`);
  }
}

async function countFiles(dir) {
  let count = 0;
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const entryPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      count += await countFiles(entryPath);
    } else {
      count += 1;
    }
  }
  return count;
}

build().catch((error) => {
  console.error(error.message || error);
  process.exitCode = 1;
});
