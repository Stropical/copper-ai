const fs = require('fs');
const path = require('path');

/**
 * Next.js static export emits absolute asset URLs like "/_next/...".
 * In KiCad's WebView we load the bundle via `file://.../out/index.html`,
 * so "/_next/..." becomes "file:///_next/..." (filesystem root) and chunk
 * loading fails.
 *
 * This script post-processes the exported `out/` directory to rewrite
 * "/_next/..." -> "./_next/..." in HTML + JS runtime files.
 */

const OUT_DIR = path.join(__dirname, 'out');
const INDEX_PATH = path.join(OUT_DIR, 'index.html');

if (!fs.existsSync(INDEX_PATH)) {
  console.error('index.html not found at:', INDEX_PATH);
  process.exit(1);
}

const TEXT_EXTS = new Set(['.html', '.js', '.css', '.json', '.txt', '.map']);

function isTextFile(filePath) {
  return TEXT_EXTS.has(path.extname(filePath).toLowerCase());
}

function walk(dir) {
  const out = [];
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) out.push(...walk(full));
    else out.push(full);
  }
  return out;
}

function rewriteNextPaths(content) {
  if (!content || typeof content !== 'string') return content;

  // The key rewrite: absolute Next assets -> relative assets.
  // Handle both double and single quotes.
  let out = content;
  out = out.replace(/"\/_next\//g, '"./_next/');
  out = out.replace(/'\/_next\//g, "'./_next/");

  // Some payloads contain escaped slashes (rare but happens in JSON blobs).
  out = out.replace(/\\\/_next\\\//g, '\\./_next/');

  return out;
}

let changedFiles = 0;
let scannedFiles = 0;

for (const filePath of walk(OUT_DIR)) {
  if (!isTextFile(filePath)) continue;
  scannedFiles++;

  let original;
  try {
    original = fs.readFileSync(filePath, 'utf8');
  } catch {
    // Skip unreadable/binary-ish files (shouldn't happen for TEXT_EXTS, but be safe).
    continue;
  }

  const updated = rewriteNextPaths(original);
  if (updated !== original) {
    fs.writeFileSync(filePath, updated, 'utf8');
    changedFiles++;
  }
}

console.log(`✓ Fixed paths for file:// protocol (scanned=${scannedFiles}, changed=${changedFiles})`);

