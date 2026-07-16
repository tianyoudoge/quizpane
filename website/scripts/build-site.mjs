#!/usr/bin/env node
// Zero-dependency static site build: copies website/src to website/dist, fingerprints
// main.css/main.js with a content hash for long-lived caching, and rewrites the
// references in index.html. No bundler, no external dependencies.

import { createHash } from "node:crypto";
import { mkdir, rm, readFile, writeFile, cp, readdir, stat } from "node:fs/promises";
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

async function build() {
  if (!(await fileExists(srcDir))) {
    throw new Error(`Missing source directory: ${srcDir}`);
  }

  await rm(distDir, { recursive: true, force: true });
  await mkdir(distDir, { recursive: true });
  await cp(srcDir, distDir, { recursive: true });

  const fingerprints = {};
  for (const name of ["main.css", "main.js"]) {
    const filePath = path.join(distDir, name);
    const contents = await readFile(filePath);
    const hash = hashOf(contents);
    const ext = path.extname(name);
    const base = path.basename(name, ext);
    const fingerprinted = `${base}.${hash}${ext}`;
    await writeFile(path.join(distDir, fingerprinted), contents);
    await rm(filePath);
    fingerprints[name] = fingerprinted;
  }

  const indexPath = path.join(distDir, "index.html");
  let html = await readFile(indexPath, "utf8");
  for (const [original, fingerprinted] of Object.entries(fingerprints)) {
    html = html.split(original).join(fingerprinted);
  }
  await writeFile(indexPath, html, "utf8");

  const fileCount = await countFiles(distDir);
  console.log(`Built website/dist (${fileCount} files).`);
  for (const [original, fingerprinted] of Object.entries(fingerprints)) {
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
