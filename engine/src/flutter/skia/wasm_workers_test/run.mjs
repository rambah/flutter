// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { createServer } from 'node:http';
import { readFile, writeFile } from 'node:fs/promises';
import { resolve, extname, basename } from 'node:path';
import { pathToFileURL } from 'node:url';

if (!process.argv[2]) throw new Error('Usage: node run.mjs BUILD_DIRECTORY');
const root = resolve(process.argv[2]);
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE
  ? pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href : 'playwright');
const results = [];
const save = () => writeFile(resolve(root, 'results.json'), JSON.stringify(results, null, 2) + '\n');
async function bounded(promise, milliseconds) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Host watchdog expired')), milliseconds);
    })]);
  } finally {
    clearTimeout(timer);
  }
}
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://127.0.0.1').pathname;
    if (path !== '/' + basename(path)) {
      res.writeHead(404); res.end(); return;
    }
    const file = await readFile(resolve(root, path.slice(1)));
    res.writeHead(200, {
      'Content-Type': ({ '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' })[extname(path)] || 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(file);
  } catch {
    res.writeHead(404); res.end();
  }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;
let browserServer, browser;
try {
  browserServer = await chromium.launchServer({ channel: 'chrome', headless: false });
  browser = await chromium.connect(browserServer.wsEndpoint());
  for (const name of ['original', 'fixed', 'fixed', 'fixed', 'fixed', 'fixed']) {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(`${base}/${name}.html`);
    await bounded(page.waitForFunction(() => globalThis.semaphoreTestResult), 25000);
    const result = await bounded(page.evaluate(() => ({
      result: globalThis.semaphoreTestResult,
      isolated: crossOriginIsolated,
      userAgent: navigator.userAgent,
    })), 5000);
    results.push({ name, ...result, errors });
    await save();
    console.log(JSON.stringify(results.at(-1)));
    await bounded(page.close(), 5000);
  }
  if (results[0].result.passed ||
      results[0].result.reason !== 'wait returned without a signal (POSIX stub)' ||
      results.some(result => !result.isolated || result.errors.length) ||
      results.slice(1).some(result => !result.result.passed)) {
    throw new Error('Expected the original implementation to fail and all patched runs to pass');
  }
} catch (error) {
  results.push({ error: error.message });
  process.exitCode = 1;
} finally {
  await save();
  try { if (browser) await bounded(browser.close(), 5000); } catch { /* Bound cleanup below. */ }
  try { if (browserServer) await bounded(browserServer.close(), 5000); }
  catch { browserServer.process()?.kill('SIGKILL'); }
  server.close();
}
