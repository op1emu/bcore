#!/usr/bin/env node
// The WebAssembly build's copy-and-patch stencils (README.md): its
// bcore-stencilgen has no threads, so this runs it in shards, one process
// per CPU, then merges what they built (tools/stencilgen.cpp's --shard and
// --merge), as one run would have.
//
//   node stencilgen.mjs GEN.js OUT_DIR [--jobs N] [stencilgen's arguments]

import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const [gen, out, ...rest] = process.argv.slice(2);
if (!gen || !out) {
  console.error("usage: stencilgen.mjs GEN.js OUT_DIR [--jobs N] [arguments]");
  process.exit(2);
}
let jobs = os.availableParallelism();
const args = [];
for (let i = 0; i < rest.length; ++i) {
  if (rest[i] === "--jobs") jobs = Math.max(1, Number(rest[++i]));
  else args.push(rest[i]);
}

function run(extra) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [gen, out, ...args, ...extra], { stdio: ["ignore", "inherit", "inherit"] });
    child.on("close", (code, signal) => resolve(signal ? 128 : code));
  });
}

fs.mkdirSync(out, { recursive: true });
const shards = (await Promise.all(Array.from({ length: jobs }, (_, k) => run(["--shard", `${k}/${jobs}`]))));
const failed = shards.findIndex((code) => code !== 0);
let code = failed >= 0 ? shards[failed] : await run(["--merge", String(jobs)]);
if (failed >= 0) console.error(`stencilgen.mjs: shard ${failed} failed (${code})`);
for (let k = 0; k < jobs; ++k) fs.rmSync(path.join(out, `shard-${k}-of-${jobs}`), { force: true });
process.exit(code);
