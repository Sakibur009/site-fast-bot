// Fast pure-JS drop-in for makefun-site 1.0.0
// v5: Fast-path inline scanner + direct slice emissions + constant-folded JIT

const REPORT_FILE = 'build-report.txt';
const RESERVED = ['posts', 'tags', 'page'];
const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December',
];

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

const HTML_SPEC = new Uint8Array(128);
HTML_SPEC[38] = 1; // &
HTML_SPEC[60] = 1; // <
HTML_SPEC[62] = 1; // >
HTML_SPEC[34] = 1; // "
HTML_SPEC[39] = 1; // '

const HTML_JSON_SPEC = new Uint8Array(128);
for (let i = 0; i < 32; i++) HTML_JSON_SPEC[i] = 1;
HTML_JSON_SPEC[34] = 1;
HTML_JSON_SPEC[38] = 1;
HTML_JSON_SPEC[39] = 1;
HTML_JSON_SPEC[60] = 1;
HTML_JSON_SPEC[62] = 1;
HTML_JSON_SPEC[92] = 1;

const JSON_SPEC = new Uint8Array(128);
for (let i = 0; i < 32; i++) JSON_SPEC[i] = 1;
JSON_SPEC[34] = 1;
JSON_SPEC[92] = 1;

const HEX = '0123456789abcdef';
function ctrlEsc(c) {
  if (c === 10) return '\\n';
  if (c === 13) return '\\r';
  if (c === 9) return '\\t';
  if (c === 8) return '\\b';
  if (c === 12) return '\\f';
  return '\\u00' + HEX[c >> 4] + HEX[c & 15];
}

function escapeHtml(s) {
  const len = s.length;
  let i = 0;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128 && HTML_SPEC[c]) break;
  }
  if (i === len) return s;
  let out = i > 0 ? s.slice(0, i) : '';
  let last = i;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128 && HTML_SPEC[c]) {
      if (i > last) out += s.slice(last, i);
      if (c === 38) out += '&amp;';
      else if (c === 60) out += '&lt;';
      else if (c === 62) out += '&gt;';
      else if (c === 34) out += '&quot;';
      else out += '&#39;';
      last = i + 1;
    }
  }
  if (last < len) out += s.slice(last);
  return out;
}

function escapeJson(s) {
  const len = s.length;
  let i = 0;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (JSON_SPEC[c]) break;
    } else if (c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < len) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i++; continue; }
      }
      return JSON.stringify(s).slice(1, -1);
    }
  }
  if (i === len) return s;
  let out = i > 0 ? s.slice(0, i) : '';
  let last = i;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (JSON_SPEC[c]) {
        if (i > last) out += s.slice(last, i);
        if (c === 34) out += '\\"';
        else if (c === 92) out += '\\\\';
        else out += ctrlEsc(c);
        last = i + 1;
      }
    } else if (c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < len) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i++; continue; }
      }
      return JSON.stringify(s).slice(1, -1);
    }
  }
  if (last < len) out += s.slice(last);
  return out;
}

function escapeHtmlJson(s) {
  const len = s.length;
  let i = 0;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (HTML_JSON_SPEC[c]) break;
    } else if (c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < len) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i++; continue; }
      }
      return JSON.stringify(escapeHtml(s)).slice(1, -1);
    }
  }
  if (i === len) return s;
  let out = i > 0 ? s.slice(0, i) : '';
  let last = i;
  for (; i < len; i++) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (HTML_JSON_SPEC[c]) {
        if (i > last) out += s.slice(last, i);
        if (c === 38) out += '&amp;';
        else if (c === 60) out += '&lt;';
        else if (c === 62) out += '&gt;';
        else if (c === 34) out += '&quot;';
        else if (c === 39) out += '&#39;';
        else if (c === 92) out += '\\\\';
        else out += ctrlEsc(c);
        last = i + 1;
      }
    } else if (c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < len) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i++; continue; }
      }
      return JSON.stringify(escapeHtml(s)).slice(1, -1);
    }
  }
  if (last < len) out += s.slice(last);
  return out;
}

function slugifyRaw(text) {
  const len = text.length;
  let ascii = true;
  for (let i = 0; i < len; i++) {
    if (text.charCodeAt(i) >= 128) { ascii = false; break; }
  }
  const plain = ascii ? text : text.normalize('NFKD').replace(/\p{M}/gu, '');
  const plen = plain.length;
  let out = '';
  let gap = false;
  for (let i = 0; i < plen; i++) {
    const c = plain.charCodeAt(i);
    let keep = -1;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep === -1) {
      if (out.length > 0) gap = true;
      continue;
    }
    if (gap) {
      if (out.length >= 63) break;
      out += '-';
      gap = false;
    }
    out += String.fromCharCode(keep);
    if (out.length >= 64) break;
  }
  if (out.length === 0) return 'untitled';
  let end = out.length;
  while (end > 0 && out.charCodeAt(end - 1) === 45) end--;
  return end === 0 ? 'untitled' : (end === out.length ? out : out.slice(0, end));
}

function uniqueSlug(slug, taken) {
  if (!taken.has(slug)) {
    taken.add(slug);
    return slug;
  }
  for (let n = 2; ; n++) {
    const out = slug + '-' + n;
    if (!taken.has(out)) {
      taken.add(out);
      return out;
    }
  }
}

function longDate(date) {
  const m = (date.charCodeAt(5) - 48) * 10 + (date.charCodeAt(6) - 48);
  const d = (date.charCodeAt(8) - 48) * 10 + (date.charCodeAt(9) - 48);
  return d + ' ' + MONTHS[m - 1] + ' ' + date.slice(0, 4);
}

function wordsOf(text) {
  const len = text.length;
  let count = 0;
  let inWord = false;
  for (let i = 0; i < len; i++) {
    const c = text.charCodeAt(i);
    if (c > 32 && c < 128) {
      if (!inWord) { inWord = true; count++; }
    } else if (c === 32 || c === 10 || c === 9 || c === 13 || c === 11 || c === 12) {
      inWord = false;
    } else if (c < 32) {
      if (!inWord) { inWord = true; count++; }
    } else if (c === 160 || c === 65279 || /\s/.test(text[i])) {
      inWord = false;
    } else if (!inWord) {
      inWord = true;
      count++;
    }
  }
  return count;
}

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  const segs = rel.split('/');
  for (let i = 0; i < segs.length; i++) {
    const seg = segs[i];
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else {
      parts.push(seg);
    }
  }
  return parts.join('/');
}

const dirOf = (path) => {
  const idx = path.lastIndexOf('/');
  return idx === -1 ? '' : path.slice(0, idx);
};

// ---------------------------------------------------------------- frontmatter

const FM_KEYS = new Set(['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary']);
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const FM_NAME = /^[a-z0-9-]+$/;
const FM_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FM_TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function fmStringOf(raw) {
  if (raw.charCodeAt(0) === 34) {
    const rlen = raw.length;
    if (rlen < 2 || raw.charCodeAt(rlen - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    let last = 1;
    for (let i = 1; i < rlen - 1; i++) {
      const c = raw.charCodeAt(i);
      if (c === 92) {
        const n = raw.charCodeAt(i + 1);
        if ((n !== 34 && n !== 92) || i + 1 === rlen - 1) {
          return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        }
        if (i > last) out += raw.slice(last, i);
        out += raw[i + 1];
        i++;
        last = i + 1;
      } else if (c === 34) {
        return { error: 'a quoted string holds a stray quote' };
      }
    }
    if (last < rlen - 1) out += raw.slice(last, rlen - 1);
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function fmValueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = fmStringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = FM_DATE.exec(raw);
      if (!m) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      const y = Number(m[1]), mo = Number(m[2]), d = Number(m[3]);
      if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) {
        return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      }
      return { value: raw };
    }
    case 'tags': {
      const rlen = raw.length;
      if (rlen < 2 || raw.charCodeAt(0) !== 91 || raw.charCodeAt(rlen - 1) !== 93) {
        return { error: 'key "tags": not a list [a, b]' };
      }
      const inner = raw.slice(1, rlen - 1);
      if (inner.trim() === '') return { value: [] };
      const parts = inner.split(',');
      const tags = new Array(parts.length);
      for (let i = 0; i < parts.length; i++) {
        const t = parts[i].trim();
        if (t === '' || !FM_TAG.test(t)) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        tags[i] = t;
      }
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return FM_NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return raw.length <= 64 && FM_SLUG.test(raw) ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

function parseFrontMatter(text) {
  const len = text.length;
  if (len < 4 || text.charCodeAt(0) !== 45 || text.charCodeAt(1) !== 45 || text.charCodeAt(2) !== 45) {
    return { error: 'no front matter' };
  }
  const nl = text.indexOf('\n');
  if (nl !== 3) return { error: nl === -1 ? 'the front matter is not closed' : 'no front matter' };

  let pos = 4;
  let closePos = -1;
  let bodyStart = len;
  while (pos <= len) {
    const nextNl = text.indexOf('\n', pos);
    const lineEnd = nextNl === -1 ? len : nextNl;
    if (lineEnd - pos === 3 && text.charCodeAt(pos) === 45 && text.charCodeAt(pos + 1) === 45 && text.charCodeAt(pos + 2) === 45) {
      closePos = pos;
      bodyStart = nextNl === -1 ? len : nextNl + 1;
      break;
    }
    if (nextNl === -1) break;
    pos = nextNl + 1;
  }
  if (closePos === -1) return { error: 'the front matter is not closed' };

  const data = {};
  pos = 4;
  let lineNum = 1;
  while (pos < closePos) {
    lineNum++;
    const nextNl = text.indexOf('\n', pos);
    const lineEnd = nextNl === -1 ? closePos : nextNl;
    let p = pos;
    while (p < lineEnd && (text.charCodeAt(p) === 32 || text.charCodeAt(p) === 9)) p++;
    if (p === lineEnd || text.charCodeAt(pos) === 35) {
      pos = lineEnd + 1;
      continue;
    }
    const line = text.slice(pos, lineEnd);
    const m = FM_LINE.exec(line);
    if (!m) return { error: `line ${lineNum} is not "key: value"` };
    const key = m[1];
    let raw = m[2] ?? '';
    let rEnd = raw.length;
    while (rEnd > 0 && (raw.charCodeAt(rEnd - 1) === 32 || raw.charCodeAt(rEnd - 1) === 9)) rEnd--;
    if (rEnd !== raw.length) raw = raw.slice(0, rEnd);
    if (!FM_KEYS.has(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = fmValueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
    pos = lineEnd + 1;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  return { data, body: bodyStart >= len ? '' : text.slice(bodyStart) };
}

// ---------------------------------------------------------------- markdown (v5 fast path)

const ESCAPABLE = new Uint8Array(128);
for (const ch of '\\`*_[]()#+-.!>') ESCAPABLE[ch.charCodeAt(0)] = 1;

const INLINE_SPEC = new Uint8Array(128);
INLINE_SPEC[92] = 1; // \
INLINE_SPEC[96] = 1; // `
INLINE_SPEC[42] = 1; // *
INLINE_SPEC[91] = 1; // [
INLINE_SPEC[38] = 1; // &
INLINE_SPEC[60] = 1; // <
INLINE_SPEC[62] = 1; // >
INLINE_SPEC[34] = 1; // "
INLINE_SPEC[39] = 1; // '

const INLINE_JSON_SPEC = new Uint8Array(128);
for (let i = 0; i < 32; i++) INLINE_JSON_SPEC[i] = 1;
INLINE_JSON_SPEC[92] = 1;
INLINE_JSON_SPEC[96] = 1;
INLINE_JSON_SPEC[42] = 1;
INLINE_JSON_SPEC[91] = 1;
INLINE_JSON_SPEC[38] = 1;
INLINE_JSON_SPEC[60] = 1;
INLINE_JSON_SPEC[62] = 1;
INLINE_JSON_SPEC[34] = 1;
INLINE_JSON_SPEC[39] = 1;

function findUnescapedChar(s, code, from, end) {
  for (let j = from; j < end; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < end) {
      const n = s.charCodeAt(j + 1);
      if (n < 128 && ESCAPABLE[n]) { j++; continue; }
    }
    if (c === code) return j;
  }
  return -1;
}

function findUnescapedDoubleStar(s, from, end) {
  const limit = end - 1;
  for (let j = from; j < limit; j++) {
    const c = s.charCodeAt(j);
    if (c === 92) {
      const n = s.charCodeAt(j + 1);
      if (n < 128 && ESCAPABLE[n]) { j++; continue; }
    }
    if (c === 42 && s.charCodeAt(j + 1) === 42) return j;
  }
  return -1;
}

function findSingleStar(s, from, end) {
  for (let j = from; j < end; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < end) {
      const n = s.charCodeAt(j + 1);
      if (n < 128 && ESCAPABLE[n]) { j++; continue; }
    }
    if (c !== 42) continue;
    if (j + 1 < end && s.charCodeAt(j + 1) === 42) { j++; continue; }
    return j;
  }
  return -1;
}

function hasWhitespace(s, start, end) {
  for (let i = start; i < end; i++) {
    const c = s.charCodeAt(i);
    if (c <= 32) {
      if (c === 32 || c === 10 || c === 9 || c === 13 || c === 11 || c === 12) return true;
    } else if (c >= 128 && /\s/.test(s[i])) {
      return true;
    }
  }
  return false;
}

function inlineSlice(s, start, end, linkFn, jsonEsc) {
  const spec = jsonEsc ? INLINE_JSON_SPEC : INLINE_SPEC;
  let i = start;
  for (; i < end; i++) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (spec[c]) break;
    } else if (jsonEsc && c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < end) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i++; continue; }
      }
      break;
    }
  }
  if (i === end) return s.slice(start, end);

  const parts = [];
  if (i > start) parts.push(s.slice(start, i));
  let last = i;

  while (i < end) {
    const c = s.charCodeAt(i);
    if (c < 128) {
      if (spec[c] === 0) { i++; continue; }
    } else if (jsonEsc && c >= 0xd800 && c <= 0xdfff) {
      if (c <= 0xdbff && i + 1 < end) {
        const c2 = s.charCodeAt(i + 1);
        if (c2 >= 0xdc00 && c2 <= 0xdfff) { i += 2; continue; }
      }
      if (i > last) parts.push(s.slice(last, i));
      parts.push('\\u' + c.toString(16).padStart(4, '0'));
      i++;
      last = i;
      continue;
    } else {
      i++;
      continue;
    }

    if (i > last) parts.push(s.slice(last, i));

    if (c === 38) { parts.push('&amp;'); i++; last = i; continue; }
    if (c === 60) { parts.push('&lt;'); i++; last = i; continue; }
    if (c === 62) { parts.push('&gt;'); i++; last = i; continue; }
    if (c === 34) { parts.push('&quot;'); i++; last = i; continue; }
    if (c === 39) { parts.push('&#39;'); i++; last = i; continue; }
    if (c < 32) { parts.push(ctrlEsc(c)); i++; last = i; continue; }

    if (c === 92) {
      if (i + 1 < end) {
        const n = s.charCodeAt(i + 1);
        if (n < 128 && ESCAPABLE[n]) {
          if (n === 62) parts.push('&gt;');
          else if (n === 92) parts.push(jsonEsc ? '\\\\' : '\\');
          else parts.push(s[i + 1]);
          i += 2;
          last = i;
          continue;
        }
      }
      parts.push(jsonEsc ? '\\\\' : '\\');
      i++;
      last = i;
      continue;
    }

    if (c === 96) {
      const j = s.indexOf('`', i + 1);
      if (j !== -1 && j < end && j > i + 1) {
        const codeSlice = s.slice(i + 1, j);
        parts.push('<code>', jsonEsc ? escapeHtmlJson(codeSlice) : escapeHtml(codeSlice), '</code>');
        i = j + 1;
      } else {
        parts.push('`');
        i++;
      }
      last = i;
      continue;
    }

    if (c === 42) {
      if (i + 1 < end && s.charCodeAt(i + 1) === 42) {
        const j = findUnescapedDoubleStar(s, i + 2, end);
        if (j > i + 2) {
          parts.push('<strong>', inlineSlice(s, i + 2, j, linkFn, jsonEsc), '</strong>');
          i = j + 2;
        } else {
          parts.push('**');
          i += 2;
        }
      } else {
        const j = findSingleStar(s, i + 1, end);
        if (j > i + 1) {
          parts.push('<em>', inlineSlice(s, i + 1, j, linkFn, jsonEsc), '</em>');
          i = j + 1;
        } else {
          parts.push('*');
          i++;
        }
      }
      last = i;
      continue;
    }

    if (c === 91) {
      const k = findUnescapedChar(s, 93, i + 1, end);
      let m = -1;
      if (k !== -1 && k + 1 < end && s.charCodeAt(k + 1) === 40) {
        m = s.indexOf(')', k + 2);
        if (m >= end) m = -1;
      }
      if (m !== -1 && m > k + 2 && !hasWhitespace(s, k + 2, m)) {
        const target = s.slice(k + 2, m);
        const href = jsonEsc ? escapeHtmlJson(linkFn(target)) : escapeHtml(linkFn(target));
        parts.push(jsonEsc ? '<a href=\\"' : '<a href="', href, jsonEsc ? '\\">' : '">', inlineSlice(s, i + 1, k, linkFn, jsonEsc), '</a>');
        i = m + 1;
      } else {
        parts.push('[');
        i++;
      }
      last = i;
      continue;
    }
  }

  if (last < end) parts.push(s.slice(last, end));
  return parts.join('');
}

function isBlankLine(src, start, end) {
  for (let i = start; i < end; i++) {
    const c = src.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function trimLineSlice(src, start, end) {
  while (start < end && (src.charCodeAt(start) === 32 || src.charCodeAt(start) === 9)) start++;
  while (end > start && (src.charCodeAt(end - 1) === 32 || src.charCodeAt(end - 1) === 9)) end--;
  return start === end ? '' : src.slice(start, end);
}

function matchFenceOpen(src, start, end) {
  if (end - start < 3 || src.charCodeAt(start) !== 96 || src.charCodeAt(start + 1) !== 96 || src.charCodeAt(start + 2) !== 96) {
    return null;
  }
  let p = start + 3;
  while (p < end) {
    const c = src.charCodeAt(p);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) {
      p++;
    } else {
      break;
    }
  }
  const langEnd = p;
  while (p < end) {
    const c = src.charCodeAt(p);
    if (c !== 32 && c !== 9) return null;
    p++;
  }
  return src.slice(start + 3, langEnd);
}

function isFenceClose(src, start, end) {
  if (end - start < 3 || src.charCodeAt(start) !== 96 || src.charCodeAt(start + 1) !== 96 || src.charCodeAt(start + 2) !== 96) {
    return false;
  }
  for (let p = start + 3; p < end; p++) {
    const c = src.charCodeAt(p);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

let _hStart = 0;
let _hEnd = 0;
function matchHeading(src, start, end) {
  if (start >= end || src.charCodeAt(start) !== 35) return 0;
  let p = start;
  while (p < end && src.charCodeAt(p) === 35 && p - start < 7) p++;
  const depth = p - start;
  if (depth < 1 || depth > 6 || p >= end) return 0;
  const sp = src.charCodeAt(p);
  if (sp !== 32 && sp !== 9) return 0;
  while (p < end && (src.charCodeAt(p) === 32 || src.charCodeAt(p) === 9)) p++;
  let e = end;
  while (e > p && (src.charCodeAt(e - 1) === 32 || src.charCodeAt(e - 1) === 9)) e--;
  if (e <= p) return 0;
  _hStart = p;
  _hEnd = e;
  return depth;
}

function isHr(src, start, end) {
  if (end - start < 3) return false;
  const c0 = src.charCodeAt(start);
  if (c0 !== 45 && c0 !== 42) return false;
  if (src.charCodeAt(start + 1) !== c0 || src.charCodeAt(start + 2) !== c0) return false;
  for (let p = start + 3; p < end; p++) {
    const c = src.charCodeAt(p);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function isOrderedStart(src, start, end) {
  let p = start;
  while (p < end && p - start < 9) {
    const c = src.charCodeAt(p);
    if (c >= 48 && c <= 57) p++;
    else break;
  }
  if (p === start || p + 1 >= end) return -1;
  if (src.charCodeAt(p) === 46 && src.charCodeAt(p + 1) === 32) return p + 2;
  return -1;
}

function isContinued(src, start, end) {
  if (end - start < 3 || src.charCodeAt(start) !== 32 || src.charCodeAt(start + 1) !== 32) return false;
  let p = start + 2;
  while (p < end && src.charCodeAt(p) === 32) p++;
  if (p >= end) return false;
  const c = src.charCodeAt(p);
  if (c <= 32) return !(c === 32 || c === 9 || c === 10 || c === 13 || c === 11 || c === 12);
  return c < 128 || !/\s/.test(src[p]);
}

function renderMarkdown(src, linkFn, slugCache, jsonEsc) {
  const len = src.length;
  const ids = new Set();
  const toc = [];
  let excerpt = null;
  const out = [];
  const NL = jsonEsc ? '\\n' : '\n';
  const Q = jsonEsc ? '\\"' : '"';

  let pos = 0;
  while (pos < len) {
    let nl = src.indexOf('\n', pos);
    let end = nl === -1 ? len : nl;
    let nextPos = nl === -1 ? len : nl + 1;

    if (isBlankLine(src, pos, end)) {
      pos = nextPos;
      continue;
    }

    const c0 = src.charCodeAt(pos);

    if (c0 === 96) {
      const lang = matchFenceOpen(src, pos, end);
      if (lang !== null) {
        out.push(lang === '' ? '<pre><code>' : '<pre><code class=' + Q + 'language-' + lang + Q + '>');
        pos = nextPos;
        while (pos < len) {
          nl = src.indexOf('\n', pos);
          end = nl === -1 ? len : nl;
          nextPos = nl === -1 ? len : nl + 1;
          if (isFenceClose(src, pos, end)) {
            pos = nextPos;
            break;
          }
          const line = src.slice(pos, end);
          out.push(jsonEsc ? escapeHtmlJson(line) : escapeHtml(line), NL);
          pos = nextPos;
        }
        out.push('</code></pre>', NL);
        continue;
      }
    }

    if (c0 === 35) {
      const depth = matchHeading(src, pos, end);
      if (depth > 0) {
        const hText = src.slice(_hStart, _hEnd);
        let baseSlug = slugCache.get(hText);
        if (baseSlug === undefined) {
          baseSlug = slugifyRaw(hText);
          slugCache.set(hText, baseSlug);
        }
        const id = uniqueSlug(baseSlug, ids);
        const hHtml = inlineSlice(src, _hStart, _hEnd, linkFn, jsonEsc);
        out.push('<h' + depth + ' id=' + Q + id + Q + '>', hHtml, '</h' + depth + '>', NL);
        if (depth === 2 || depth === 3) toc.push({ level: depth, id, html: hHtml });
        pos = nextPos;
        continue;
      }
    }

    if ((c0 === 45 || c0 === 42) && isHr(src, pos, end)) {
      out.push('<hr>', NL);
      pos = nextPos;
      continue;
    }

    if (c0 === 62) {
      const qLines = [];
      while (pos < len) {
        nl = src.indexOf('\n', pos);
        end = nl === -1 ? len : nl;
        nextPos = nl === -1 ? len : nl + 1;
        if (pos >= end || src.charCodeAt(pos) !== 62) break;
        let qStart = pos + 1;
        if (qStart < end && src.charCodeAt(qStart) === 32) qStart++;
        const trimmed = trimLineSlice(src, qStart, end);
        if (trimmed !== '') qLines.push(trimmed);
        pos = nextPos;
      }
      if (qLines.length === 0) {
        out.push('<blockquote>', NL, '</blockquote>', NL);
      } else {
        const qText = qLines.join('\n');
        out.push('<blockquote>', NL, '<p>', inlineSlice(qText, 0, qText.length, linkFn, jsonEsc), '</p>', NL, '</blockquote>', NL);
      }
      continue;
    }

    if ((c0 === 45 || c0 === 42) && pos + 1 < end && src.charCodeAt(pos + 1) === 32) {
      out.push('<ul>', NL);
      const itemLines = [trimLineSlice(src, pos + 2, end)];
      pos = nextPos;
      while (pos < len) {
        nl = src.indexOf('\n', pos);
        end = nl === -1 ? len : nl;
        nextPos = nl === -1 ? len : nl + 1;
        const ch = src.charCodeAt(pos);
        if ((ch === 45 || ch === 42) && pos + 1 < end && src.charCodeAt(pos + 1) === 32) {
          const itemStr = itemLines.join('\n');
          out.push('<li>', inlineSlice(itemStr, 0, itemStr.length, linkFn, jsonEsc), '</li>', NL);
          itemLines.length = 0;
          itemLines.push(trimLineSlice(src, pos + 2, end));
          pos = nextPos;
        } else if (isContinued(src, pos, end)) {
          itemLines.push(trimLineSlice(src, pos, end));
          pos = nextPos;
        } else {
          break;
        }
      }
      const lastItemStr = itemLines.join('\n');
      out.push('<li>', inlineSlice(lastItemStr, 0, lastItemStr.length, linkFn, jsonEsc), '</li>', NL, '</ul>', NL);
      continue;
    }

    if (c0 >= 48 && c0 <= 57) {
      const restStart = isOrderedStart(src, pos, end);
      if (restStart !== -1) {
        const startNum = Number(src.slice(pos, restStart - 2));
        out.push(startNum !== 1 ? '<ol start=' + Q + startNum + Q + '>' + NL : '<ol>' + NL);
        const itemLines = [trimLineSlice(src, restStart, end)];
        pos = nextPos;
        while (pos < len) {
          nl = src.indexOf('\n', pos);
          end = nl === -1 ? len : nl;
          nextPos = nl === -1 ? len : nl + 1;
          const rStart = src.charCodeAt(pos) >= 48 && src.charCodeAt(pos) <= 57 ? isOrderedStart(src, pos, end) : -1;
          if (rStart !== -1) {
            const itemStr = itemLines.join('\n');
            out.push('<li>', inlineSlice(itemStr, 0, itemStr.length, linkFn, jsonEsc), '</li>', NL);
            itemLines.length = 0;
            itemLines.push(trimLineSlice(src, rStart, end));
            pos = nextPos;
          } else if (isContinued(src, pos, end)) {
            itemLines.push(trimLineSlice(src, pos, end));
            pos = nextPos;
          } else {
            break;
          }
        }
        const lastItemStr = itemLines.join('\n');
        out.push('<li>', inlineSlice(lastItemStr, 0, lastItemStr.length, linkFn, jsonEsc), '</li>', NL, '</ol>', NL);
        continue;
      }
    }

    const pLines = [trimLineSlice(src, pos, end)];
    pos = nextPos;
    while (pos < len) {
      nl = src.indexOf('\n', pos);
      end = nl === -1 ? len : nl;
      nextPos = nl === -1 ? len : nl + 1;
      if (
        isBlankLine(src, pos, end) ||
        (src.charCodeAt(pos) === 96 && matchFenceOpen(src, pos, end) !== null) ||
        (src.charCodeAt(pos) === 35 && matchHeading(src, pos, end) > 0) ||
        ((src.charCodeAt(pos) === 45 || src.charCodeAt(pos) === 42) && isHr(src, pos, end))
      ) {
        break;
      }
      pLines.push(trimLineSlice(src, pos, end));
      pos = nextPos;
    }
    const pText = pLines.join('\n');
    const pHtml = inlineSlice(pText, 0, pText.length, linkFn, jsonEsc);
    if (excerpt === null) excerpt = pHtml;
    out.push('<p>', pHtml, '</p>', NL);
  }

  return { html: out.join(''), toc, excerpt: excerpt ?? '' };
}

// ---------------------------------------------------------------- templates (Constant-Folded JIT)

const INCLUDE_DEPTH = 8;
const T_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const T_FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const T_IF = /^if (not )?(\S+)$/;
const T_INCLUDE = /^include "([a-z0-9-]+)"$/;

class TemplateError extends Error {}

function pathOf(name, text) {
  if (!T_PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  const len = src.length;
  while (i < len) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === len - 1) {
      text += src.slice(i);
      break;
    }
    const next = src.charCodeAt(open + 1);
    if (next !== 123 && next !== 37) {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === 123) {
      const raw = src.charCodeAt(open + 2) === 123;
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? len : nl;
    let standalone = true;
    for (let k = lineStart; k < open; k++) {
      const c = src.charCodeAt(k);
      if (c !== 32 && c !== 9) { standalone = false; break; }
    }
    if (standalone) {
      for (let k = end; k < lineEnd; k++) {
        const c = src.charCodeAt(k);
        if (c !== 32 && c !== 9) { standalone = false; break; }
      }
    }
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? len : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = T_FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = T_IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = T_INCLUDE.exec(tag)) !== null) {
        body.push({ kind: 'include', name: m[1] });
      } else {
        throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
      }
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

function usesEscapedMdField(nodes) {
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (n.kind === 'var') {
      const last = n.path[n.path.length - 1];
      if (last === 'content' || last === 'excerpt' || last === 'html') return true;
    } else if (n.kind === 'if') {
      if (usesEscapedMdField(n.then)) return true;
      if (n.else !== null && usesEscapedMdField(n.else)) return true;
    } else if (n.kind === 'for') {
      if (usesEscapedMdField(n.body)) return true;
    }
  }
  return false;
}

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return '' + v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) {
    let r = '';
    for (let i = 0; i < v.length; i++) {
      if (i > 0) r += ', ';
      r += textOf(v[i]);
    }
    return r;
  }
  return '';
}

function escTextOf(v) {
  if (typeof v === 'string') return escapeHtml(v);
  if (typeof v === 'number') return '' + v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return escapeHtml(textOf(v));
  return '';
}

function rawJsonTextOf(v) {
  if (typeof v === 'string') return escapeJson(v);
  if (typeof v === 'number') return '' + v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return escapeJson(textOf(v));
  return '';
}

function escJsonTextOf(v) {
  if (typeof v === 'string') return escapeHtmlJson(v);
  if (typeof v === 'number') return '' + v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return escapeHtmlJson(textOf(v));
  return '';
}

function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

function lookupPath(root, path) {
  let v = root;
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

function lookupScope(scope, path) {
  let s = scope;
  while (s !== null && !Object.hasOwn(s.vars, path[0])) s = s.parent;
  if (s === null) return undefined;
  let v = s.vars[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

function compileTemplate(nodes, templates, constVars, jsonMode, preEscMd) {
  let uid = 0;
  const code = ['let _o="";'];
  const lits = [];
  const constScopeRoot = { vars: constVars, parent: null };

  function addLit(s) {
    const idx = lits.length;
    lits.push(s);
    return '_L[' + idx + ']';
  }

  function isMdField(path) {
    const last = path[path.length - 1];
    return last === 'content' || last === 'excerpt' || last === 'html';
  }

  function isConstPath(path, dynScopes) {
    const p0 = path[0];
    for (let i = dynScopes.length - 1; i >= 0; i--) {
      if (p0 === dynScopes[i].name || p0 === 'loop') return false;
    }
    return Object.hasOwn(constVars, p0);
  }

  function isSubtreeConst(list, localConstNames, dynScopes, depth) {
    for (let i = 0; i < list.length; i++) {
      const n = list[i];
      if (n.kind === 'text') continue;
      if (n.kind === 'include') {
        const inc = templates.get(n.name);
        if (inc !== undefined && depth < INCLUDE_DEPTH) {
          if (!isSubtreeConst(inc, localConstNames, dynScopes, depth + 1)) return false;
        }
        continue;
      }
      const p0 = n.path[0];
      const isLocal = localConstNames.has(p0);
      if (!isLocal && !isConstPath(n.path, dynScopes)) return false;
      if (n.kind === 'if') {
        if (!isSubtreeConst(n.then, localConstNames, dynScopes, depth)) return false;
        if (n.else !== null && !isSubtreeConst(n.else, localConstNames, dynScopes, depth)) return false;
      } else if (n.kind === 'for') {
        const nextLocal = new Set(localConstNames);
        nextLocal.add(n.name);
        nextLocal.add('loop');
        if (!isSubtreeConst(n.body, nextLocal, dynScopes, depth)) return false;
      }
    }
    return true;
  }

  function evalConstNodes(list, scope, depth) {
    let out = '';
    for (let i = 0; i < list.length; i++) {
      const n = list[i];
      if (n.kind === 'text') {
        out += jsonMode ? escapeJson(n.text) : n.text;
      } else if (n.kind === 'var') {
        const v = lookupScope(scope, n.path);
        out += jsonMode ? escJsonTextOf(v) : escTextOf(v);
      } else if (n.kind === 'raw') {
        const v = lookupScope(scope, n.path);
        if (preEscMd && isMdField(n.path)) out += typeof v === 'string' ? v : rawJsonTextOf(v);
        else out += jsonMode ? rawJsonTextOf(v) : textOf(v);
      } else if (n.kind === 'if') {
        const v = truthy(lookupScope(scope, n.path));
        const br = v !== n.not ? n.then : n.else;
        if (br !== null) out += evalConstNodes(br, scope, depth);
      } else if (n.kind === 'for') {
        const xs = lookupScope(scope, n.path);
        if (Array.isArray(xs)) {
          for (let k = 0; k < xs.length; k++) {
            const vars = { [n.name]: xs[k], loop: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length } };
            out += evalConstNodes(n.body, { vars, parent: scope }, depth);
          }
        }
      } else if (n.kind === 'include') {
        const inc = templates.get(n.name);
        if (inc !== undefined && depth < INCLUDE_DEPTH) {
          out += evalConstNodes(inc, scope, depth + 1);
        }
      }
    }
    return out;
  }

  function resolveExpr(path, dynScopes) {
    const p0 = path[0];
    let base = null;
    for (let i = dynScopes.length - 1; i >= 0; i--) {
      const sc = dynScopes[i];
      if (p0 === sc.name) {
        base = sc.varId;
        break;
      }
      if (p0 === 'loop') {
        if (path.length === 2) {
          const prop = path[1];
          if (prop === 'index') return '(' + sc.idxId + '+1)';
          if (prop === 'first') return '(' + sc.idxId + '===0)';
          if (prop === 'last') return '(' + sc.idxId + '===' + sc.lenId + '-1)';
          if (prop === 'length') return sc.lenId;
          return 'undefined';
        }
        base = '{index:' + sc.idxId + '+1,first:' + sc.idxId + '===0,last:' + sc.idxId + '===' + sc.lenId + '-1,length:' + sc.lenId + '}';
        break;
      }
    }
    if (base === null) {
      base = '(_H(_R,' + JSON.stringify(p0) + ')?_R[' + JSON.stringify(p0) + ']:undefined)';
    }
    if (path.length === 1) return base;
    if (path.length === 2) {
      const k1 = JSON.stringify(path[1]);
      const vId = '_v' + (uid++);
      code.push('let ' + vId + '=' + base + ';');
      return '(' + vId + '!==null&&typeof ' + vId + '==="object"&&_H(' + vId + ',' + k1 + ')?' + vId + '[' + k1 + ']:undefined)';
    }
    const pIdx = lits.length;
    lits.push(path);
    return '_P(' + base + ',_L[' + pIdx + '])';
  }

  let pendingText = '';
  function flushText() {
    if (pendingText !== '') {
      code.push('_o+=' + addLit(pendingText) + ';');
      pendingText = '';
    }
  }

  function emit(list, dynScopes, depth) {
    for (let i = 0; i < list.length; i++) {
      const n = list[i];
      if (n.kind === 'text') {
        pendingText += jsonMode ? escapeJson(n.text) : n.text;
      } else if (n.kind === 'include') {
        const inc = templates.get(n.name);
        if (inc !== undefined && depth < INCLUDE_DEPTH) {
          emit(inc, dynScopes, depth + 1);
        }
      } else if (n.kind === 'var') {
        if (isConstPath(n.path, dynScopes)) {
          const v = lookupScope(constScopeRoot, n.path);
          pendingText += jsonMode ? escJsonTextOf(v) : escTextOf(v);
        } else {
          flushText();
          const expr = resolveExpr(n.path, dynScopes);
          code.push('_o+=_E(' + expr + ');');
        }
      } else if (n.kind === 'raw') {
        if (isConstPath(n.path, dynScopes)) {
          const v = lookupScope(constScopeRoot, n.path);
          if (preEscMd && isMdField(n.path)) pendingText += typeof v === 'string' ? v : rawJsonTextOf(v);
          else pendingText += jsonMode ? rawJsonTextOf(v) : textOf(v);
        } else {
          flushText();
          const expr = resolveExpr(n.path, dynScopes);
          if (preEscMd && isMdField(n.path)) {
            const vId = '_m' + (uid++);
            code.push('const ' + vId + '=' + expr + ';if(typeof ' + vId + '==="string")_o+=' + vId + ';else _o+=_T(' + vId + ');');
          } else {
            code.push('_o+=_T(' + expr + ');');
          }
        }
      } else if (n.kind === 'if') {
        if (isConstPath(n.path, dynScopes)) {
          const v = truthy(lookupScope(constScopeRoot, n.path));
          const br = v !== n.not ? n.then : n.else;
          if (br !== null) emit(br, dynScopes, depth);
        } else {
          flushText();
          const expr = resolveExpr(n.path, dynScopes);
          code.push('if(' + (n.not ? '!' : '') + '_B(' + expr + ')){');
          emit(n.then, dynScopes, depth);
          flushText();
          if (n.else !== null) {
            code.push('}else{');
            emit(n.else, dynScopes, depth);
            flushText();
          }
          code.push('}');
        }
      } else if (n.kind === 'for') {
        if (isConstPath(n.path, dynScopes) && isSubtreeConst(n.body, new Set([n.name, 'loop']), dynScopes, depth)) {
          const xs = lookupScope(constScopeRoot, n.path);
          if (Array.isArray(xs)) {
            for (let k = 0; k < xs.length; k++) {
              const vars = { [n.name]: xs[k], loop: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length } };
              pendingText += evalConstNodes(n.body, { vars, parent: constScopeRoot }, depth);
            }
          }
        } else {
          flushText();
          const expr = resolveExpr(n.path, dynScopes);
          const id = uid++;
          const arrId = '_a' + id;
          const idxId = '_i' + id;
          const lenId = '_l' + id;
          const varId = '_x' + id;
          code.push('const ' + arrId + '=' + expr + ';if(Array.isArray(' + arrId + ')){const ' + lenId + '=' + arrId + '.length;for(let ' + idxId + '=0;' + idxId + '<' + lenId + ';' + idxId + '++){const ' + varId + '=' + arrId + '[' + idxId + '];');
          dynScopes.push({ name: n.name, varId, idxId, lenId });
          emit(n.body, dynScopes, depth);
          flushText();
          dynScopes.pop();
          code.push('}}');
        }
      }
    }
  }

  emit(nodes, [], 0);
  flushText();
  code.push('return _o;');
  const fn = new Function('_R', '_L', '_E', '_T', '_B', '_P', '_H', code.join(''));
  const eFn = jsonMode ? escJsonTextOf : escTextOf;
  const tFn = jsonMode ? rawJsonTextOf : textOf;
  return (root) => fn(root, lits, eFn, tFn, truthy, lookupPath, Object.hasOwn);
}

// ---------------------------------------------------------------- core build

const SCHEME_RE = /^[A-Za-z][A-Za-z0-9+.-]*:/;

function buildCore(input, writeStr) {
  const jsonMode = writeStr !== null;
  const templates = new Map();
  const report = [];
  let preEscMd = jsonMode;

  const tplNames = Object.keys(input.templates).sort(byCodeUnits);
  for (let i = 0; i < tplNames.length; i++) {
    const name = tplNames[i];
    try {
      const nodes = parseTemplate(name, input.templates[name]);
      templates.set(name, nodes);
      if (preEscMd && usesEscapedMdField(nodes)) preEscMd = false;
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }

  let base = input.site.base_url;
  let bEnd = base.length;
  while (bEnd > 0 && base.charCodeAt(bEnd - 1) === 47) bEnd--;
  if (bEnd !== base.length) base = base.slice(0, bEnd);

  const files = input.files.slice().sort((x, y) => byCodeUnits(x.path, y.path));
  const sources = [];
  const statics = [];
  const staticPaths = new Set();

  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    const p = f.path;
    if (p.endsWith('.md')) {
      const raw = f.content;
      const text = raw.indexOf('\r') === -1 ? raw : raw.replace(/\r\n?/g, '\n');
      sources.push({ path: p, text, data: null, body: '', post: false, layout: '', slug: '', outDir: '', url: '', html: '', toc: null, excerpt: '', newer: null, older: null, tagRefs: null });
    } else {
      statics.push(f);
      staticPaths.add(p);
    }
  }

  const kept = [];
  for (let i = 0; i < sources.length; i++) {
    const s = sources[i];
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      report.push('refused ' + s.path + ': ' + fm.error);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    if (!s.post) {
      if (s.path.startsWith('tags/')) {
        report.push('refused ' + s.path + ': tags/ is the generator\'s own');
        continue;
      }
      if (s.path.startsWith('page/')) {
        report.push('refused ' + s.path + ': page/ is the generator\'s own');
        continue;
      }
    }
    if (s.post && !Object.hasOwn(s.data, 'date')) {
      report.push('refused ' + s.path + ': a post needs a date');
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!templates.has(layout)) {
      report.push('refused ' + s.path + ': layout "' + layout + '" is no template that parses');
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      report.push('draft ' + s.path);
      continue;
    }
    kept.push(s);
  }

  const slugCache = new Map();
  const slugify = (text) => {
    let v = slugCache.get(text);
    if (v === undefined) {
      v = slugifyRaw(text);
      slugCache.set(text, v);
    }
    return v;
  };

  const taken = new Map();
  const bySource = new Map();
  for (let i = 0; i < kept.length; i++) {
    const s = kept[i];
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) {
      set = new Set(dir === '' ? RESERVED : undefined);
      taken.set(dir, set);
    }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : dir + '/' + slug;
    s.url = base + '/' + s.outDir + '/';
    bySource.set(s.path, s);
  }

  const broken = new Set();
  for (let i = 0; i < kept.length; i++) {
    const s = kept[i];
    const dir = dirOf(s.path);
    const linkFn = (target) => {
      const c0 = target.charCodeAt(0);
      if (c0 === 47 || c0 === 35 || SCHEME_RE.test(target)) return target;
      const hash = target.indexOf('#');
      const rel = hash === -1 ? target : target.slice(0, hash);
      const frag = hash === -1 ? '' : target.slice(hash);
      const path = rel === '' ? null : resolvePath(dir, rel);
      if (path !== null) {
        const targetSrc = bySource.get(path);
        if (targetSrc !== undefined) return targetSrc.url + frag;
        if (staticPaths.has(path)) return base + '/' + path + frag;
      }
      broken.add('broken-link ' + s.path + ' -> ' + target);
      return target;
    };
    const md = renderMarkdown(s.body, linkFn, slugCache, preEscMd);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  const posts = [];
  for (let i = 0; i < kept.length; i++) {
    if (kept[i].post) posts.push(kept[i]);
  }
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  const bySlug = new Map();
  for (let i = 0; i < posts.length; i++) {
    const p = posts[i];
    const pTags = p.data.tags;
    const refs = [];
    if (pTags !== undefined && pTags.length > 0) {
      const seen = new Set();
      for (let j = 0; j < pTags.length; j++) {
        const name = pTags[j];
        const slug = slugify(name);
        if (seen.has(slug)) continue;
        seen.add(slug);
        let tag = bySlug.get(slug);
        if (tag === undefined) {
          tag = { name, slug, url: base + '/tags/' + slug + '/', posts: [] };
          bySlug.set(slug, tag);
        }
        tag.posts.push(p);
        refs.push(tag);
      }
    }
    p.tagRefs = refs;
  }
  for (let i = 0; i < kept.length; i++) {
    const s = kept[i];
    if (s.post) continue;
    const sTags = s.data.tags;
    const refs = [];
    if (sTags !== undefined && sTags.length > 0) {
      const seen = new Set();
      for (let j = 0; j < sTags.length; j++) {
        const name = sTags[j];
        const slug = slugify(name);
        if (seen.has(slug)) continue;
        seen.add(slug);
        refs.push(bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
      }
    }
    s.tagRefs = refs;
  }
  const tagsList = Array.from(bySlug.values()).sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  const cardOf = (s) => {
    const tr = s.tagRefs;
    const tOut = new Array(tr.length);
    for (let i = 0; i < tr.length; i++) {
      const t = tr[i];
      tOut[i] = { name: t.name, slug: t.slug, url: t.url };
    }
    const dt = s.data.date;
    return {
      title: s.data.title,
      url: s.url,
      date: dt ?? '',
      date_long: dt === undefined ? '' : longDate(dt),
      summary: s.data.summary ?? '',
      excerpt: s.excerpt,
      reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
      tags: tOut,
    };
  };

  const cards = new Map();
  const postCards = new Array(posts.length);
  for (let i = 0; i < posts.length; i++) {
    const c = cardOf(posts[i]);
    cards.set(posts[i], c);
    postCards[i] = c;
  }

  const linkOf = (s) => {
    if (s === null) return null;
    const c = cards.get(s);
    if (c !== undefined) return { title: c.title, url: c.url, date: c.date, date_long: c.date_long };
    const dt = s.data.date;
    return { title: s.data.title, url: s.url, date: dt ?? '', date_long: dt === undefined ? '' : longDate(dt) };
  };

  const site = { title: input.site.title, base_url: base };
  const allTags = new Array(tagsList.length);
  for (let i = 0; i < tagsList.length; i++) {
    const t = tagsList[i];
    allTags[i] = { name: t.name, slug: t.slug, url: t.url, count: t.posts.length };
  }
  const recent = postCards.slice(0, input.site.recent);
  const constVars = { site, tags: allTags, recent };

  const compiled = new Map();
  function getCompiled(name) {
    let fn = compiled.get(name);
    if (fn !== undefined) return fn;
    const nodes = templates.get(name);
    if (nodes === undefined) {
      compiled.set(name, null);
      return null;
    }
    fn = compileTemplate(nodes, templates, constVars, jsonMode, preEscMd);
    compiled.set(name, fn);
    return fn;
  }

  const outObj = jsonMode ? null : {};
  const written = jsonMode ? new Set() : null;
  let firstKey = true;

  function emitFile(path, content, alreadyJson) {
    if (jsonMode) {
      written.add(path);
      const prefix = firstKey ? '{"' : ',"';
      const val = alreadyJson ? content : escapeJson(content);
      writeStr(prefix + escapeJson(path) + '":"' + val + '"');
      firstKey = false;
    } else {
      outObj[path] = content;
    }
  }

  for (let i = 0; i < kept.length; i++) {
    const s = kept[i];
    const fn = getCompiled(s.layout);
    const c = s.post ? cards.get(s) : cardOf(s);
    const pageObj = {
      title: c.title,
      url: c.url,
      date: c.date,
      date_long: c.date_long,
      summary: c.summary,
      excerpt: c.excerpt,
      reading_minutes: c.reading_minutes,
      tags: c.tags,
      slug: s.slug,
      content: s.html,
      toc: s.toc,
      newer: linkOf(s.newer),
      older: linkOf(s.older),
      is_post: s.post,
    };
    emitFile(s.outDir + '/index.html', fn({ site, tags: allTags, recent, page: pageObj }), jsonMode);
  }

  const indexFn = getCompiled('index');
  if (indexFn !== null) {
    const per = input.site.per_page;
    const pages = Math.max(1, Math.ceil(posts.length / per));
    const indexUrl = (k) => (k === 1 ? base + '/' : base + '/page/' + k + '/');
    for (let k = 1; k <= pages; k++) {
      const html = indexFn({
        site,
        tags: allTags,
        recent,
        posts: postCards.slice((k - 1) * per, k * per),
        pager: {
          number: k,
          total: pages,
          url: indexUrl(k),
          newer_url: k === 1 ? '' : indexUrl(k - 1),
          older_url: k === pages ? '' : indexUrl(k + 1),
        },
      });
      emitFile(k === 1 ? 'index.html' : 'page/' + k + '/index.html', html, jsonMode);
    }
  }

  const tagFn = getCompiled('tag');
  if (tagFn !== null) {
    for (let i = 0; i < tagsList.length; i++) {
      const t = tagsList[i];
      const tPosts = new Array(t.posts.length);
      for (let j = 0; j < t.posts.length; j++) tPosts[j] = cards.get(t.posts[j]);
      emitFile('tags/' + t.slug + '/index.html', tagFn({
        site,
        tags: allTags,
        recent,
        tag: { name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: tPosts },
      }), jsonMode);
    }
  }

  const tagsFn = getCompiled('tags');
  if (tagsFn !== null) {
    emitFile('tags/index.html', tagsFn({ site, tags: allTags, recent }), jsonMode);
  }

  for (let i = 0; i < statics.length; i++) {
    const f = statics[i];
    const exists = jsonMode ? written.has(f.path) : Object.hasOwn(outObj, f.path);
    if (exists) report.push('shadowed ' + f.path + ': a built page has its path');
    else emitFile(f.path, f.content, false);
  }

  const lines = report.concat(Array.from(broken)).sort(byCodeUnits);
  const pageCount = kept.length - posts.length;
  lines.push('built ' + pageCount + ' pages, ' + posts.length + ' posts, ' + tagsList.length + ' tags and ' + statics.length + ' static files');
  emitFile(REPORT_FILE, lines.join('\n') + '\n', false);

  if (jsonMode) {
    writeStr('}');
    return null;
  }
  return outObj;
}

export function run(input) {
  return buildCore(input, null);
}

export function runToBuf(input, writeStr) {
  buildCore(input, writeStr);
}