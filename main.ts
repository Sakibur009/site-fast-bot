import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

setFlagsFromString('--no-incremental-marking');

import { runToBuf } from './solve.js';

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const cases = JSON.parse(readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8'));

let cap = 16 * 1024 * 1024;
let buf = Buffer.allocUnsafe(cap);
let pos = 0;

function ensure(need) {
  if (pos + need <= cap) return;
  while (pos + need > cap) cap *= 2;
  const next = Buffer.allocUnsafe(cap);
  buf.copy(next, 0, 0, pos);
  buf = next;
}

function writeStr(s) {
  const maxBytes = s.length * 3;
  if (pos + maxBytes > cap) ensure(maxBytes);
  pos += buf.write(s, pos, 'utf8');
}

buf[pos++] = 91; // [
for (let i = 0; i < cases.length; i++) {
  if (i > 0) {
    if (pos + 1 > cap) ensure(1);
    buf[pos++] = 44; // ,
  }
  runToBuf(cases[i].input, writeStr);
}
if (pos + 1 > cap) ensure(1);
buf[pos++] = 93; // ]

writeFileSync(join(OUTPUT_DIR, 'answers.json'), buf.subarray(0, pos));