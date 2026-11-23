import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const wasmBytes = readFileSync(new URL('./engine.wasm', import.meta.url));
const raw = readFileSync(join(INPUT_DIR, 'cases.json'));

const dec = new TextDecoder();
const enc = new TextEncoder();
let exp: any;

const inst = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), {
  env: {
    js_slugify(ptr: number, len: number, outPtr: number): number {
      const s = dec.decode(new Uint8Array(exp.memory.buffer, ptr, len));
      const plain = s.normalize('NFKD').replace(/\p{M}/gu, '');
      let out = '', gap = false;
      for (let i = 0; i < plain.length; i++) {
        const c = plain.charCodeAt(i);
        let k = -1;
        if (c >= 65 && c <= 90) k = c + 32;
        else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) k = c;
        if (k === -1) { if (out.length > 0) gap = true; continue; }
        if (gap) { if (out.length >= 63) break; out += '-'; gap = false; }
        out += String.fromCharCode(k);
        if (out.length >= 64) break;
      }
      let end = out.length;
      while (end > 0 && out.charCodeAt(end - 1) === 45) end--;
      const res = end === 0 ? 'untitled' : out.slice(0, end);
      const b = enc.encode(res);
      new Uint8Array(exp.memory.buffer, outPtr, b.length).set(b);
      return b.length;
    }
  }
});

exp = inst.exports;
const inPtr = exp.alloc_input(raw.length);
new Uint8Array(exp.memory.buffer, inPtr, raw.length).set(raw);
const outLen = exp.solve_all(inPtr, raw.length);
const outPtr = exp.get_out_ptr();
writeFileSync(join(OUTPUT_DIR, 'answers.json'), new Uint8Array(exp.memory.buffer, outPtr, outLen));