#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wasm_simd128.h>

__attribute__((import_module("env"), import_name("js_slugify")))
extern uint32_t js_slugify(const char *p, uint32_t n, char *out);

extern unsigned char __heap_base;

void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n >= 16) {
    wasm_v128_store(d, wasm_v128_load(s));
    d += 16; s += 16; n -= 16;
  }
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else if (d > s) {
    d += n; s += n;
    while (n--) *--d = *--s;
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  uint8_t v = (uint8_t)c;
  v128_t vv = wasm_i8x16_splat((int8_t)v);
  while (n >= 16) {
    wasm_v128_store(d, vv);
    d += 16; n -= 16;
  }
  while (n--) *d++ = v;
  return dst;
}

static inline int my_memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}

// ---------------- Bump Arena ----------------
static uint8_t *heap_ptr = NULL;
static uint8_t *heap_end = NULL;

static void arena_init(void) {
  if (!heap_ptr) {
    heap_ptr = &__heap_base;
    heap_end = (uint8_t *)(__builtin_wasm_memory_size(0) * 65536ULL);
  }
}

static void *arena_alloc(size_t n) {
  uintptr_t p = ((uintptr_t)heap_ptr + 7u) & ~(uintptr_t)7u;
  size_t need = (p - (uintptr_t)heap_ptr) + n;
  if ((size_t)(heap_end - heap_ptr) < need) {
    size_t missing = need - (size_t)(heap_end - heap_ptr);
    size_t pages = (missing + 65535u) / 65536u;
    if (pages < 256) pages = 256;
    __builtin_wasm_memory_grow(0, pages);
    heap_end = (uint8_t *)(__builtin_wasm_memory_size(0) * 65536ULL);
  }
  heap_ptr = (uint8_t *)(p + n);
  return (void *)p;
}

static void *arena_calloc(size_t n) {
  void *p = arena_alloc(n);
  memset(p, 0, n);
  return p;
}

// ---------------- Str & Buf ----------------
typedef struct { const char *p; uint32_t n; } Str;

static inline Str str_lit(const char *s, uint32_t n) { Str r = {s, n}; return r; }
#define S(lit) str_lit(lit, (uint32_t)(sizeof(lit) - 1))

static inline bool str_eq(Str a, Str b) {
  return a.n == b.n && (a.n == 0 || my_memcmp(a.p, b.p, a.n) == 0);
}

static inline int str_cmp(Str a, Str b) {
  uint32_t m = a.n < b.n ? a.n : b.n;
  int r = my_memcmp(a.p, b.p, m);
  if (r != 0) return r;
  return a.n < b.n ? -1 : (a.n > b.n ? 1 : 0);
}

static inline bool str_starts(Str a, Str pref) {
  return a.n >= pref.n && my_memcmp(a.p, pref.p, pref.n) == 0;
}

static inline bool str_ends(Str a, Str suf) {
  return a.n >= suf.n && my_memcmp(a.p + a.n - suf.n, suf.p, suf.n) == 0;
}

static uint64_t str_hash(Str s) {
  uint64_t h = 14695981039346656037ULL;
  for (uint32_t i = 0; i < s.n; i++) {
    h ^= (uint8_t)s.p[i];
    h *= 1099511628211ULL;
  }
  return h ? h : 1;
}

typedef struct { char *d; uint32_t n; uint32_t cap; } Buf;

static void buf_init(Buf *b, uint32_t cap) {
  b->d = (char *)arena_alloc(cap);
  b->n = 0;
  b->cap = cap;
}

static inline void buf_grow(Buf *b, uint32_t add) {
  if (b->n + add <= b->cap) return;
  uint32_t ncap = b->cap ? b->cap * 2 : 256;
  while (ncap < b->n + add) ncap *= 2;
  char *nd = (char *)arena_alloc(ncap);
  if (b->n) memcpy(nd, b->d, b->n);
  b->d = nd;
  b->cap = ncap;
}

static inline void buf_putc(Buf *b, char c) {
  if (b->n + 1 > b->cap) buf_grow(b, 1);
  b->d[b->n++] = c;
}

static inline void buf_put(Buf *b, const char *s, uint32_t n) {
  if (!n) return;
  if (b->n + n > b->cap) buf_grow(b, n);
  memcpy(b->d + b->n, s, n);
  b->n += n;
}

static inline void buf_puts(Buf *b, Str s) { buf_put(b, s.p, s.n); }

static void buf_put_u32(Buf *b, uint32_t v) {
  char tmp[16];
  int k = 0;
  if (v == 0) { buf_putc(b, '0'); return; }
  while (v > 0) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
  buf_grow(b, (uint32_t)k);
  while (k > 0) b->d[b->n++] = tmp[--k];
}

static void buf_put_i32(Buf *b, int32_t v) {
  if (v < 0) { buf_putc(b, '-'); buf_put_u32(b, (uint32_t)(-v)); }
  else buf_put_u32(b, (uint32_t)v);
}

static const char HEX[] = "0123456789abcdef";

static void buf_put_ctrl(Buf *b, uint8_t c) {
  if (c == '\n') buf_put(b, "\\n", 2);
  else if (c == '\r') buf_put(b, "\\r", 2);
  else if (c == '\t') buf_put(b, "\\t", 2);
  else if (c == '\b') buf_put(b, "\\b", 2);
  else if (c == '\f') buf_put(b, "\\f", 2);
  else {
    char tmp[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15]};
    buf_put(b, tmp, 6);
  }
}

// SIMD128 JSON escape
static void buf_put_json(Buf *b, const char *s, uint32_t n) {
  if (!n) return;
  buf_grow(b, n * 2 + 16);
  v128_t v_sp = wasm_i8x16_splat(32);
  v128_t v_qt = wasm_i8x16_splat('"');
  v128_t v_bs = wasm_i8x16_splat('\\');
  uint32_t i = 0;
  while (i + 16 <= n) {
    v128_t v = wasm_v128_load(s + i);
    v128_t m = wasm_v128_or(wasm_u8x16_lt(v, v_sp),
               wasm_v128_or(wasm_i8x16_eq(v, v_qt), wasm_i8x16_eq(v, v_bs)));
    int32_t mask = wasm_i8x16_bitmask(m);
    if (mask == 0) {
      if (b->n + 16 > b->cap) buf_grow(b, 16);
      wasm_v128_store(b->d + b->n, v);
      b->n += 16;
      i += 16;
    } else {
      uint32_t tz = (uint32_t)__builtin_ctz((unsigned)mask);
      if (tz) { buf_put(b, s + i, tz); i += tz; }
      uint8_t c = (uint8_t)s[i++];
      if (c == '"') buf_put(b, "\\\"", 2);
      else if (c == '\\') buf_put(b, "\\\\", 2);
      else buf_put_ctrl(b, c);
    }
  }
  uint32_t last = i;
  for (; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 32 || c == '"' || c == '\\') {
      if (i > last) buf_put(b, s + last, i - last);
      if (c == '"') buf_put(b, "\\\"", 2);
      else if (c == '\\') buf_put(b, "\\\\", 2);
      else buf_put_ctrl(b, c);
      last = i + 1;
    }
  }
  if (last < n) buf_put(b, s + last, n - last);
}

// SIMD128 HTML + JSON escape
static void buf_put_html_json(Buf *b, const char *s, uint32_t n) {
  if (!n) return;
  buf_grow(b, n * 2 + 16);
  v128_t v_sp = wasm_i8x16_splat(32);
  v128_t v_qt = wasm_i8x16_splat('"');
  v128_t v_ap = wasm_i8x16_splat('\'');
  v128_t v_am = wasm_i8x16_splat('&');
  v128_t v_lt = wasm_i8x16_splat('<');
  v128_t v_gt = wasm_i8x16_splat('>');
  v128_t v_bs = wasm_i8x16_splat('\\');
  uint32_t i = 0;
  while (i + 16 <= n) {
    v128_t v = wasm_v128_load(s + i);
    v128_t m = wasm_v128_or(wasm_u8x16_lt(v, v_sp),
               wasm_v128_or(wasm_i8x16_eq(v, v_qt),
               wasm_v128_or(wasm_i8x16_eq(v, v_ap),
               wasm_v128_or(wasm_i8x16_eq(v, v_am),
               wasm_v128_or(wasm_i8x16_eq(v, v_lt),
               wasm_v128_or(wasm_i8x16_eq(v, v_gt), wasm_i8x16_eq(v, v_bs)))))));
    int32_t mask = wasm_i8x16_bitmask(m);
    if (mask == 0) {
      if (b->n + 16 > b->cap) buf_grow(b, 16);
      wasm_v128_store(b->d + b->n, v);
      b->n += 16;
      i += 16;
    } else {
      uint32_t tz = (uint32_t)__builtin_ctz((unsigned)mask);
      if (tz) { buf_put(b, s + i, tz); i += tz; }
      uint8_t c = (uint8_t)s[i++];
      if (c == '&') buf_put(b, "&amp;", 5);
      else if (c == '<') buf_put(b, "&lt;", 4);
      else if (c == '>') buf_put(b, "&gt;", 4);
      else if (c == '"') buf_put(b, "&quot;", 6);
      else if (c == '\'') buf_put(b, "&#39;", 5);
      else if (c == '\\') buf_put(b, "\\\\", 2);
      else buf_put_ctrl(b, c);
    }
  }
  uint32_t last = i;
  for (; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 32 || c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' || c == '\\') {
      if (i > last) buf_put(b, s + last, i - last);
      if (c == '&') buf_put(b, "&amp;", 5);
      else if (c == '<') buf_put(b, "&lt;", 4);
      else if (c == '>') buf_put(b, "&gt;", 4);
      else if (c == '"') buf_put(b, "&quot;", 6);
      else if (c == '\'') buf_put(b, "&#39;", 5);
      else if (c == '\\') buf_put(b, "\\\\", 2);
      else buf_put_ctrl(b, c);
      last = i + 1;
    }
  }
  if (last < n) buf_put(b, s + last, n - last);
}

static void buf_put_html(Buf *b, const char *s, uint32_t n) {
  if (!n) return;
  uint32_t last = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') {
      if (i > last) buf_put(b, s + last, i - last);
      if (c == '&') buf_put(b, "&amp;", 5);
      else if (c == '<') buf_put(b, "&lt;", 4);
      else if (c == '>') buf_put(b, "&gt;", 4);
      else if (c == '"') buf_put(b, "&quot;", 6);
      else buf_put(b, "&#39;", 5);
      last = i + 1;
    }
  }
  if (last < n) buf_put(b, s + last, n - last);
}

// ---------------- Hash Map (Str -> void*) ----------------
typedef struct { Str k; void *v; uint64_t h; } MapEnt;
typedef struct { MapEnt *e; uint32_t cap; uint32_t len; } StrMap;

static void map_init(StrMap *m, uint32_t cap) {
  uint32_t c = 16;
  while (c < cap * 2) c <<= 1;
  m->e = (MapEnt *)arena_calloc(c * sizeof(MapEnt));
  m->cap = c;
  m->len = 0;
}

static void map_grow(StrMap *m) {
  uint32_t ocap = m->cap;
  MapEnt *oe = m->e;
  m->cap = ocap ? ocap * 2 : 32;
  m->e = (MapEnt *)arena_calloc(m->cap * sizeof(MapEnt));
  uint32_t mask = m->cap - 1;
  for (uint32_t i = 0; i < ocap; i++) {
    if (oe[i].h) {
      uint32_t idx = (uint32_t)oe[i].h & mask;
      while (m->e[idx].h) idx = (idx + 1) & mask;
      m->e[idx] = oe[i];
    }
  }
}

static void *map_get(const StrMap *m, Str k) {
  if (!m->cap) return NULL;
  uint64_t h = str_hash(k);
  uint32_t mask = m->cap - 1;
  uint32_t idx = (uint32_t)h & mask;
  while (m->e[idx].h) {
    if (m->e[idx].h == h && str_eq(m->e[idx].k, k)) return m->e[idx].v;
    idx = (idx + 1) & mask;
  }
  return NULL;
}

static bool map_has(const StrMap *m, Str k) {
  if (!m->cap) return false;
  uint64_t h = str_hash(k);
  uint32_t mask = m->cap - 1;
  uint32_t idx = (uint32_t)h & mask;
  while (m->e[idx].h) {
    if (m->e[idx].h == h && str_eq(m->e[idx].k, k)) return true;
    idx = (idx + 1) & mask;
  }
  return false;
}

static bool map_set(StrMap *m, Str k, void *v) {
  if ((m->len + 1) * 2 >= m->cap) map_grow(m);
  uint64_t h = str_hash(k);
  uint32_t mask = m->cap - 1;
  uint32_t idx = (uint32_t)h & mask;
  while (m->e[idx].h) {
    if (m->e[idx].h == h && str_eq(m->e[idx].k, k)) {
      m->e[idx].v = v;
      return false;
    }
    idx = (idx + 1) & mask;
  }
  m->e[idx].k = k;
  m->e[idx].v = v;
  m->e[idx].h = h;
  m->len++;
  return true;
}

// ---------------- JSON Parser (SIMD128 Bulk Unescape) ----------------
typedef struct { const char *s; uint32_t n; uint32_t i; } JP;

static inline void jp_ws(JP *p) {
  while (p->i < p->n) {
    uint8_t c = (uint8_t)p->s[p->i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') p->i++;
    else break;
  }
}

static inline int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

static inline uint32_t parse_u4(const char *s) {
  return (uint32_t)((hex_val(s[0]) << 12) | (hex_val(s[1]) << 8) | (hex_val(s[2]) << 4) | hex_val(s[3]));
}

static void put_utf8(Buf *b, uint32_t cp) {
  if (cp < 0x80) {
    buf_putc(b, (char)cp);
  } else if (cp < 0x800) {
    buf_putc(b, (char)(0xC0 | (cp >> 6)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    buf_putc(b, (char)(0xE0 | (cp >> 12)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    buf_putc(b, (char)(0xF0 | (cp >> 18)));
    buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(b, (char)(0x80 | (cp & 0x3F)));
  }
}

static Str jp_str(JP *p) {
  jp_ws(p);
  p->i++; // skip '"'
  uint32_t start = p->i;
  v128_t v_qt = wasm_i8x16_splat('"');
  v128_t v_bs = wasm_i8x16_splat('\\');
  while (p->i + 16 <= p->n) {
    v128_t v = wasm_v128_load(p->s + p->i);
    int32_t m = wasm_i8x16_bitmask(wasm_v128_or(wasm_i8x16_eq(v, v_qt), wasm_i8x16_eq(v, v_bs)));
    if (m == 0) p->i += 16;
    else { p->i += (uint32_t)__builtin_ctz((unsigned)m); break; }
  }
  while (p->i < p->n && p->s[p->i] != '"' && p->s[p->i] != '\\') p->i++;
  if (p->i < p->n && p->s[p->i] == '"') {
    Str r = { p->s + start, p->i - start };
    p->i++;
    return r;
  }
  uint32_t init_cap = (p->i - start) + 4096;
  if (init_cap > (p->n - start) + 16) init_cap = (p->n - start) + 16;
  Buf b;
  buf_init(&b, init_cap);
  if (p->i > start) buf_put(&b, p->s + start, p->i - start);

  while (p->i < p->n) {
    char c = p->s[p->i++];
    if (c == '"') break;
    if (c == '\\') {
      char e = p->s[p->i++];
      if (e == '"' || e == '\\' || e == '/') buf_putc(&b, e);
      else if (e == 'n') buf_putc(&b, '\n');
      else if (e == 'r') buf_putc(&b, '\r');
      else if (e == 't') buf_putc(&b, '\t');
      else if (e == 'b') buf_putc(&b, '\b');
      else if (e == 'f') buf_putc(&b, '\f');
      else if (e == 'u') {
        uint32_t cp = parse_u4(p->s + p->i);
        p->i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
          uint32_t lo = parse_u4(p->s + p->i + 2);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            p->i += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
        }
        put_utf8(&b, cp);
      }
      // SIMD scan for next '"' or '\'
      uint32_t run_s = p->i;
      while (p->i + 16 <= p->n) {
        v128_t v = wasm_v128_load(p->s + p->i);
        int32_t m = wasm_i8x16_bitmask(wasm_v128_or(wasm_i8x16_eq(v, v_qt), wasm_i8x16_eq(v, v_bs)));
        if (m == 0) p->i += 16;
        else { p->i += (uint32_t)__builtin_ctz((unsigned)m); break; }
      }
      while (p->i < p->n && p->s[p->i] != '"' && p->s[p->i] != '\\') p->i++;
      if (p->i > run_s) buf_put(&b, p->s + run_s, p->i - run_s);
    } else {
      buf_putc(&b, c);
    }
  }
  return str_lit(b.d, b.n);
}

static int32_t jp_int(JP *p) {
  jp_ws(p);
  bool neg = false;
  if (p->i < p->n && p->s[p->i] == '-') { neg = true; p->i++; }
  int32_t v = 0;
  while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
    v = v * 10 + (p->s[p->i++] - '0');
  }
  return neg ? -v : v;
}

static void jp_skip(JP *p) {
  jp_ws(p);
  if (p->i >= p->n) return;
  char c = p->s[p->i];
  if (c == '"') {
    p->i++;
    while (p->i < p->n) {
      if (p->s[p->i] == '\\') p->i += 2;
      else if (p->s[p->i] == '"') { p->i++; break; }
      else p->i++;
    }
  } else if (c == '{') {
    p->i++;
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; return; }
    while (p->i < p->n) {
      jp_skip(p);
      jp_ws(p);
      if (p->i < p->n && p->s[p->i] == ':') p->i++;
      jp_skip(p);
      jp_ws(p);
      if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
      if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
    }
  } else if (c == '[') {
    p->i++;
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; return; }
    while (p->i < p->n) {
      jp_skip(p);
      jp_ws(p);
      if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
      if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
    }
  } else {
    while (p->i < p->n) {
      char ch = p->s[p->i];
      if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') break;
      p->i++;
    }
  }
}

// ---------------- Data Models ----------------
typedef struct { Str path; Str content; } InputFile;
typedef struct { Str name; Str src; } InputTpl;
typedef struct {
  Str title; Str base_url; int32_t per_page; int32_t recent;
  InputTpl *tpls; uint32_t n_tpls;
  InputFile *files; uint32_t n_files;
} SiteInput;

static SiteInput parse_site_input(JP *p) {
  SiteInput in;
  memset(&in, 0, sizeof(in));
  jp_ws(p);
  p->i++; // '{'
  jp_ws(p);
  if (p->i < p->n && p->s[p->i] == '}') { p->i++; return in; }
  while (p->i < p->n) {
    Str k = jp_str(p);
    jp_ws(p); p->i++; // ':'
    if (str_eq(k, S("site"))) {
      jp_ws(p); p->i++; // '{'
      jp_ws(p);
      while (p->i < p->n && p->s[p->i] != '}') {
        Str sk = jp_str(p);
        jp_ws(p); p->i++;
        if (str_eq(sk, S("title"))) in.title = jp_str(p);
        else if (str_eq(sk, S("base_url"))) in.base_url = jp_str(p);
        else if (str_eq(sk, S("per_page"))) in.per_page = jp_int(p);
        else if (str_eq(sk, S("recent"))) in.recent = jp_int(p);
        else jp_skip(p);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') p->i++;
        jp_ws(p);
      }
      if (p->i < p->n) p->i++;
    } else if (str_eq(k, S("templates"))) {
      uint32_t cap = 32;
      in.tpls = (InputTpl *)arena_alloc(cap * sizeof(InputTpl));
      jp_ws(p); p->i++; // '{'
      jp_ws(p);
      while (p->i < p->n && p->s[p->i] != '}') {
        if (in.n_tpls == cap) {
          cap *= 2;
          InputTpl *nt = (InputTpl *)arena_alloc(cap * sizeof(InputTpl));
          memcpy(nt, in.tpls, in.n_tpls * sizeof(InputTpl));
          in.tpls = nt;
        }
        Str tk = jp_str(p);
        jp_ws(p); p->i++;
        Str tv = jp_str(p);
        in.tpls[in.n_tpls].name = tk;
        in.tpls[in.n_tpls].src = tv;
        in.n_tpls++;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') p->i++;
        jp_ws(p);
      }
      if (p->i < p->n) p->i++;
    } else if (str_eq(k, S("files"))) {
      uint32_t cap = 256;
      in.files = (InputFile *)arena_alloc(cap * sizeof(InputFile));
      jp_ws(p); p->i++; // '['
      jp_ws(p);
      while (p->i < p->n && p->s[p->i] != ']') {
        if (in.n_files == cap) {
          cap *= 2;
          InputFile *nf = (InputFile *)arena_alloc(cap * sizeof(InputFile));
          memcpy(nf, in.files, in.n_files * sizeof(InputFile));
          in.files = nf;
        }
        InputFile f = { {0,0}, {0,0} };
        jp_ws(p); p->i++; // '{'
        jp_ws(p);
        while (p->i < p->n && p->s[p->i] != '}') {
          Str fk = jp_str(p);
          jp_ws(p); p->i++;
          if (str_eq(fk, S("path"))) f.path = jp_str(p);
          else if (str_eq(fk, S("content"))) f.content = jp_str(p);
          else jp_skip(p);
          jp_ws(p);
          if (p->i < p->n && p->s[p->i] == ',') p->i++;
          jp_ws(p);
        }
        if (p->i < p->n) p->i++; // '}'
        in.files[in.n_files++] = f;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') p->i++;
        jp_ws(p);
      }
      if (p->i < p->n) p->i++; // ']'
    } else {
      jp_skip(p);
    }
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
  }
  return in;
}

// ---------------- Utilities ----------------
static const char *MONTHS[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};
static const uint32_t MONTH_LEN[12] = {7,8,5,5,3,4,4,6,9,7,8,8};

static Str long_date(Str d) {
  if (d.n < 10) return str_lit("", 0);
  int m = (d.p[5] - '0') * 10 + (d.p[6] - '0');
  int day = (d.p[8] - '0') * 10 + (d.p[9] - '0');
  Buf b; buf_init(&b, 32);
  buf_put_u32(&b, (uint32_t)day);
  buf_putc(&b, ' ');
  buf_put(&b, MONTHS[m - 1], MONTH_LEN[m - 1]);
  buf_putc(&b, ' ');
  buf_put(&b, d.p, 4);
  return str_lit(b.d, b.n);
}

static Str slugify_raw(Str text) {
  bool ascii = true;
  for (uint32_t i = 0; i < text.n; i++) {
    if ((uint8_t)text.p[i] >= 128) { ascii = false; break; }
  }
  if (!ascii) {
    char *out = (char *)arena_alloc(128);
    uint32_t len = js_slugify(text.p, text.n, out);
    return str_lit(out, len);
  }
  char *out = (char *)arena_alloc(64);
  uint32_t on = 0;
  bool gap = false;
  for (uint32_t i = 0; i < text.n; i++) {
    uint8_t c = (uint8_t)text.p[i];
    int keep = -1;
    if (c >= 'A' && c <= 'Z') keep = c + 32;
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
    if (keep == -1) {
      if (on > 0) gap = true;
      continue;
    }
    if (gap) {
      if (on >= 63) break;
      out[on++] = '-';
      gap = false;
    }
    out[on++] = (char)keep;
    if (on >= 64) break;
  }
  while (on > 0 && out[on - 1] == '-') on--;
  if (on == 0) return S("untitled");
  return str_lit(out, on);
}

static Str slugify_cached(Str text, StrMap *cache) {
  Str *found = (Str *)map_get(cache, text);
  if (found) return *found;
  Str *val = (Str *)arena_alloc(sizeof(Str));
  *val = slugify_raw(text);
  map_set(cache, text, val);
  return *val;
}

static Str unique_slug(Str slug, StrMap *taken) {
  if (!map_has(taken, slug)) {
    map_set(taken, slug, (void *)1);
    return slug;
  }
  for (uint32_t n = 2; ; n++) {
    Buf b; buf_init(&b, slug.n + 12);
    buf_puts(&b, slug);
    buf_putc(&b, '-');
    buf_put_u32(&b, n);
    Str cand = str_lit(b.d, b.n);
    if (!map_has(taken, cand)) {
      map_set(taken, cand, (void *)1);
      return cand;
    }
  }
}

static uint32_t words_of(Str text) {
  uint32_t count = 0;
  bool in_word = false;
  for (uint32_t i = 0; i < text.n; i++) {
    uint8_t c = (uint8_t)text.p[i];
    if (c > 32 && c < 128) {
      if (!in_word) { in_word = true; count++; }
    } else if (c == 32 || c == 10 || c == 9 || c == 13 || c == 11 || c == 12) {
      in_word = false;
    } else if (c < 32) {
      if (!in_word) { in_word = true; count++; }
    } else if (c == 0xC2 && i + 1 < text.n && (uint8_t)text.p[i + 1] == 0xA0) {
      in_word = false; i++;
    } else if (c == 0xEF && i + 2 < text.n && (uint8_t)text.p[i + 1] == 0xBB && (uint8_t)text.p[i + 2] == 0xBF) {
      in_word = false; i += 2;
    } else {
      if (!in_word) { in_word = true; count++; }
    }
  }
  return count;
}

static Str dir_of(Str path) {
  for (int32_t i = (int32_t)path.n - 1; i >= 0; i--) {
    if (path.p[i] == '/') return str_lit(path.p, (uint32_t)i);
  }
  return str_lit("", 0);
}

static bool resolve_path(Str dir, Str rel, Str *out) {
  Str parts[64];
  uint32_t np = 0;
  if (dir.n > 0) {
    uint32_t s = 0;
    for (uint32_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (np < 64) parts[np++] = str_lit(dir.p + s, i - s);
        s = i + 1;
      }
    }
  }
  uint32_t s = 0;
  for (uint32_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Str seg = str_lit(rel.p + s, i - s);
      s = i + 1;
      if (seg.n == 0 || (seg.n == 1 && seg.p[0] == '.')) continue;
      if (seg.n == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
        if (np == 0) return false;
        np--;
      } else {
        if (np < 64) parts[np++] = seg;
      }
    }
  }
  Buf b; buf_init(&b, dir.n + rel.n + 2);
  for (uint32_t i = 0; i < np; i++) {
    if (i > 0) buf_putc(&b, '/');
    buf_puts(&b, parts[i]);
  }
  *out = str_lit(b.d, b.n);
  return true;
}

// ---------------- Frontmatter ----------------
typedef struct {
  bool has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
  Str title, date, layout, slug, summary;
  bool draft;
  Str *tags; uint32_t n_tags;
} FMData;

static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

static bool fm_str(Str raw, Str *out, Str *err) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') { *err = S("a quoted string is not closed"); return false; }
    Buf b; buf_init(&b, raw.n);
    uint32_t last = 1;
    for (uint32_t i = 1; i < raw.n - 1; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        if (i + 1 >= raw.n - 1 || (raw.p[i + 1] != '"' && raw.p[i + 1] != '\\')) {
          *err = S("a quoted string holds an escape other than \\\" or \\\\");
          return false;
        }
        if (i > last) buf_put(&b, raw.p + last, i - last);
        buf_putc(&b, raw.p[i + 1]);
        i++;
        last = i + 1;
      } else if (c == '"') {
        *err = S("a quoted string holds a stray quote");
        return false;
      }
    }
    if (last < raw.n - 1) buf_put(&b, raw.p + last, (raw.n - 1) - last);
    *out = str_lit(b.d, b.n);
    return true;
  }
  if (raw.n > 0 && raw.p[0] == '[') { *err = S("a bare string opens on ["); return false; }
  *out = raw;
  return true;
}

static bool is_name_ok(Str s) {
  if (s.n == 0) return false;
  for (uint32_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

static bool is_slug_ok(Str s) {
  if (s.n == 0 || s.n > 64) return false;
  bool prev_dash = true;
  for (uint32_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) prev_dash = false;
    else if (c == '-' && !prev_dash && i + 1 < s.n) prev_dash = true;
    else return false;
  }
  return true;
}

static Str fmt_err2(Str a, Str b, Str c) {
  Buf buf; buf_init(&buf, a.n + b.n + c.n + 1);
  buf_puts(&buf, a); buf_puts(&buf, b); buf_puts(&buf, c);
  return str_lit(buf.d, buf.n);
}

static bool parse_frontmatter(Str text, FMData *out, Str *body, Str *err) {
  memset(out, 0, sizeof(*out));
  if (text.n < 3 || text.p[0] != '-' || text.p[1] != '-' || text.p[2] != '-') {
    *err = S("no front matter");
    return false;
  }
  int32_t nl = -1;
  for (uint32_t i = 0; i < text.n; i++) { if (text.p[i] == '\n') { nl = (int32_t)i; break; } }
  if (nl != 3) {
    *err = (nl == -1) ? S("the front matter is not closed") : S("no front matter");
    return false;
  }
  uint32_t pos = 4;
  int32_t close_pos = -1;
  uint32_t body_start = text.n;
  while (pos <= text.n) {
    uint32_t le = pos;
    while (le < text.n && text.p[le] != '\n') le++;
    if (le - pos == 3 && text.p[pos] == '-' && text.p[pos + 1] == '-' && text.p[pos + 2] == '-') {
      close_pos = (int32_t)pos;
      body_start = (le < text.n) ? le + 1 : text.n;
      break;
    }
    if (le == text.n) break;
    pos = le + 1;
  }
  if (close_pos == -1) { *err = S("the front matter is not closed"); return false; }

  pos = 4;
  uint32_t line_num = 1;
  while (pos < (uint32_t)close_pos) {
    line_num++;
    uint32_t le = pos;
    while (le < (uint32_t)close_pos && text.p[le] != '\n') le++;
    uint32_t p = pos;
    while (p < le && (text.p[p] == ' ' || text.p[p] == '\t')) p++;
    if (p == le || text.p[pos] == '#') { pos = le + 1; continue; }

    uint32_t kp = pos;
    while (kp < le && ((text.p[kp] >= 'a' && text.p[kp] <= 'z') || text.p[kp] == '_')) kp++;
    if (kp == pos || kp >= le || text.p[kp] != ':' || (kp + 1 < le && text.p[kp + 1] != ' ')) {
      Buf eb; buf_init(&eb, 48);
      buf_puts(&eb, S("line ")); buf_put_u32(&eb, line_num); buf_puts(&eb, S(" is not \"key: value\""));
      *err = str_lit(eb.d, eb.n);
      return false;
    }
    Str key = str_lit(text.p + pos, kp - pos);
    uint32_t vs = (kp + 1 < le) ? kp + 2 : le;
    uint32_t ve = le;
    while (ve > vs && (text.p[ve - 1] == ' ' || text.p[ve - 1] == '\t')) ve--;
    Str raw = str_lit(text.p + vs, ve - vs);

    if (str_eq(key, S("title"))) {
      if (out->has_title) { *err = S("key \"title\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"title\" has no value"); return false; }
      Str se;
      if (!fm_str(raw, &out->title, &se)) { *err = fmt_err2(S("key \"title\": "), se, S("")); return false; }
      out->has_title = true;
    } else if (str_eq(key, S("summary"))) {
      if (out->has_summary) { *err = S("key \"summary\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"summary\" has no value"); return false; }
      Str se;
      if (!fm_str(raw, &out->summary, &se)) { *err = fmt_err2(S("key \"summary\": "), se, S("")); return false; }
      out->has_summary = true;
    } else if (str_eq(key, S("date"))) {
      if (out->has_date) { *err = S("key \"date\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"date\" has no value"); return false; }
      bool ok = raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-';
      for (int k = 0; ok && k < 10; k++) if (k != 4 && k != 7 && (raw.p[k] < '0' || raw.p[k] > '9')) ok = false;
      if (ok) {
        int y = (raw.p[0]-'0')*1000 + (raw.p[1]-'0')*100 + (raw.p[2]-'0')*10 + (raw.p[3]-'0');
        int mo = (raw.p[5]-'0')*10 + (raw.p[6]-'0');
        int d = (raw.p[8]-'0')*10 + (raw.p[9]-'0');
        if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y, mo)) ok = false;
      }
      if (!ok) { *err = S("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"); return false; }
      out->date = raw; out->has_date = true;
    } else if (str_eq(key, S("tags"))) {
      if (out->has_tags) { *err = S("key \"tags\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"tags\" has no value"); return false; }
      if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
        *err = S("key \"tags\": not a list [a, b]"); return false;
      }
      uint32_t is = 1, ie = raw.n - 1;
      while (is < ie && (raw.p[is] == ' ' || raw.p[is] == '\t')) is++;
      while (ie > is && (raw.p[ie - 1] == ' ' || raw.p[ie - 1] == '\t')) ie--;
      if (is == ie) {
        out->tags = NULL; out->n_tags = 0;
      } else {
        uint32_t cnt = 1;
        for (uint32_t k = is; k < ie; k++) if (raw.p[k] == ',') cnt++;
        out->tags = (Str *)arena_alloc(cnt * sizeof(Str));
        uint32_t ts = 1, ti = 0;
        for (uint32_t k = 1; k <= raw.n - 1; k++) {
          if (k == raw.n - 1 || raw.p[k] == ',') {
            uint32_t a = ts, b = k;
            while (a < b && (raw.p[a] == ' ' || raw.p[a] == '\t')) a++;
            while (b > a && (raw.p[b - 1] == ' ' || raw.p[b - 1] == '\t')) b--;
            if (a == b) { *err = S("key \"tags\": an empty tag, or one holding \" [ or ]"); return false; }
            for (uint32_t q = a; q < b; q++) {
              if (raw.p[q] == '"' || raw.p[q] == '[' || raw.p[q] == ']') {
                *err = S("key \"tags\": an empty tag, or one holding \" [ or ]"); return false;
              }
            }
            out->tags[ti++] = str_lit(raw.p + a, b - a);
            ts = k + 1;
          }
        }
        out->n_tags = ti;
      }
      out->has_tags = true;
    } else if (str_eq(key, S("draft"))) {
      if (out->has_draft) { *err = S("key \"draft\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"draft\" has no value"); return false; }
      if (str_eq(raw, S("true"))) out->draft = true;
      else if (str_eq(raw, S("false"))) out->draft = false;
      else { *err = S("key \"draft\": true or false"); return false; }
      out->has_draft = true;
    } else if (str_eq(key, S("layout"))) {
      if (out->has_layout) { *err = S("key \"layout\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"layout\" has no value"); return false; }
      if (!is_name_ok(raw)) { *err = S("key \"layout\": not a template name"); return false; }
      out->layout = raw; out->has_layout = true;
    } else if (str_eq(key, S("slug"))) {
      if (out->has_slug) { *err = S("key \"slug\" is given twice"); return false; }
      if (raw.n == 0) { *err = S("key \"slug\" has no value"); return false; }
      if (!is_slug_ok(raw)) { *err = S("key \"slug\": not a slug"); return false; }
      out->slug = raw; out->has_slug = true;
    } else {
      *err = fmt_err2(S("unknown key \""), key, S("\""));
      return false;
    }
    pos = le + 1;
  }
  if (!out->has_title) { *err = S("no title"); return false; }
  *body = (body_start >= text.n) ? str_lit("", 0) : str_lit(text.p + body_start, text.n - body_start);
  return true;
}

// ---------------- Markdown (SIMD128 Inline Scanner) ----------------
static uint8_t ESC_TBL[128];
static uint8_t INL_TBL[128];
static uint8_t INL_J_TBL[128];
static bool md_tables_init = false;

static void init_md_tables(void) {
  if (md_tables_init) return;
  md_tables_init = true;
  const char *es = "\\`*_[]()#+-.!>";
  for (int i = 0; es[i]; i++) ESC_TBL[(uint8_t)es[i]] = 1;
  const char *sp = "\\`*[&<>\"'";
  for (int i = 0; sp[i]; i++) { INL_TBL[(uint8_t)sp[i]] = 1; INL_J_TBL[(uint8_t)sp[i]] = 1; }
  for (int i = 0; i < 32; i++) INL_J_TBL[i] = 1;
}

typedef struct { int32_t level; Str id; Str html; } TocItem;

typedef struct SourceDoc SourceDoc;
typedef struct TagObj TagObj;

struct SourceDoc {
  Str path, text, body, layout, slug, out_dir, url, html, excerpt;
  FMData fm;
  bool is_post;
  TocItem *toc; uint32_t n_toc;
  SourceDoc *newer, *older;
  TagObj **tag_refs; uint32_t n_tag_refs;
  uint32_t reading_minutes;
  Str date_long;
};

struct TagObj {
  Str name, slug, url;
  SourceDoc **posts; uint32_t n_posts; uint32_t cap_posts;
};

typedef struct {
  Str cur_path;
  Str cur_dir;
  Str base_url;
  StrMap *by_source;
  StrMap *static_paths;
  StrMap *broken;
  Str *broken_list; uint32_t n_broken; uint32_t cap_broken;
} LinkCtx;

static bool has_scheme(Str s) {
  if (s.n == 0) return false;
  char c0 = s.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
  for (uint32_t i = 1; i < s.n; i++) {
    char c = s.p[i];
    if (c == ':') return true;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')) return false;
  }
  return false;
}

static Str resolve_link(LinkCtx *lc, Str target) {
  if (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#' || has_scheme(target))) return target;
  int32_t hash = -1;
  for (uint32_t i = 0; i < target.n; i++) { if (target.p[i] == '#') { hash = (int32_t)i; break; } }
  Str rel = (hash == -1) ? target : str_lit(target.p, (uint32_t)hash);
  Str frag = (hash == -1) ? str_lit("", 0) : str_lit(target.p + hash, target.n - (uint32_t)hash);
  Str resolved;
  if (rel.n > 0 && resolve_path(lc->cur_dir, rel, &resolved)) {
    SourceDoc *ts = (SourceDoc *)map_get(lc->by_source, resolved);
    if (ts) {
      if (frag.n == 0) return ts->url;
      Buf b; buf_init(&b, ts->url.n + frag.n);
      buf_puts(&b, ts->url); buf_puts(&b, frag);
      return str_lit(b.d, b.n);
    }
    if (map_has(lc->static_paths, resolved)) {
      Buf b; buf_init(&b, lc->base_url.n + 1 + resolved.n + frag.n);
      buf_puts(&b, lc->base_url); buf_putc(&b, '/'); buf_puts(&b, resolved); buf_puts(&b, frag);
      return str_lit(b.d, b.n);
    }
  }
  Buf eb; buf_init(&eb, 12 + lc->cur_path.n + 4 + target.n);
  buf_puts(&eb, S("broken-link ")); buf_puts(&eb, lc->cur_path); buf_puts(&eb, S(" -> ")); buf_puts(&eb, target);
  Str msg = str_lit(eb.d, eb.n);
  if (map_set(lc->broken, msg, (void *)1)) {
    if (lc->n_broken == lc->cap_broken) {
      lc->cap_broken = lc->cap_broken ? lc->cap_broken * 2 : 32;
      Str *nl = (Str *)arena_alloc(lc->cap_broken * sizeof(Str));
      if (lc->n_broken) memcpy(nl, lc->broken_list, lc->n_broken * sizeof(Str));
      lc->broken_list = nl;
    }
    lc->broken_list[lc->n_broken++] = msg;
  }
  return target;
}

static int32_t find_unesc_char(const char *s, char ch, uint32_t from, uint32_t end) {
  for (uint32_t j = from; j < end; j++) {
    uint8_t c = (uint8_t)s[j];
    if (c == '\\' && j + 1 < end && (uint8_t)s[j + 1] < 128 && ESC_TBL[(uint8_t)s[j + 1]]) { j++; continue; }
    if (c == (uint8_t)ch) return (int32_t)j;
  }
  return -1;
}

static int32_t find_unesc_dstar(const char *s, uint32_t from, uint32_t end) {
  if (end < 2) return -1;
  uint32_t lim = end - 1;
  for (uint32_t j = from; j < lim; j++) {
    uint8_t c = (uint8_t)s[j];
    if (c == '\\' && (uint8_t)s[j + 1] < 128 && ESC_TBL[(uint8_t)s[j + 1]]) { j++; continue; }
    if (c == '*' && s[j + 1] == '*') return (int32_t)j;
  }
  return -1;
}

static int32_t find_single_star(const char *s, uint32_t from, uint32_t end) {
  for (uint32_t j = from; j < end; j++) {
    uint8_t c = (uint8_t)s[j];
    if (c == '\\' && j + 1 < end && (uint8_t)s[j + 1] < 128 && ESC_TBL[(uint8_t)s[j + 1]]) { j++; continue; }
    if (c != '*') continue;
    if (j + 1 < end && s[j + 1] == '*') { j++; continue; }
    return (int32_t)j;
  }
  return -1;
}

static bool has_ws_range(const char *s, uint32_t a, uint32_t b) {
  for (uint32_t i = a; i < b; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\v' || c == '\f') return true;
  }
  return false;
}

static void render_inline(Buf *out, const char *s, uint32_t start, uint32_t end, LinkCtx *lc, bool json_esc) {
  const uint8_t *tbl = json_esc ? INL_J_TBL : INL_TBL;
  v128_t v_sp = wasm_i8x16_splat(32);
  v128_t v_bs = wasm_i8x16_splat('\\');
  v128_t v_bt = wasm_i8x16_splat('`');
  v128_t v_st = wasm_i8x16_splat('*');
  v128_t v_lb = wasm_i8x16_splat('[');
  v128_t v_am = wasm_i8x16_splat('&');
  v128_t v_lt = wasm_i8x16_splat('<');
  v128_t v_gt = wasm_i8x16_splat('>');
  v128_t v_qt = wasm_i8x16_splat('"');
  v128_t v_ap = wasm_i8x16_splat('\'');

  uint32_t i = start, last = start;
  while (i < end) {
    if (i == last) {
      while (i + 16 <= end) {
        v128_t v = wasm_v128_load(s + i);
        v128_t m = wasm_v128_or(wasm_i8x16_eq(v, v_bs),
                   wasm_v128_or(wasm_i8x16_eq(v, v_bt),
                   wasm_v128_or(wasm_i8x16_eq(v, v_st),
                   wasm_v128_or(wasm_i8x16_eq(v, v_lb),
                   wasm_v128_or(wasm_i8x16_eq(v, v_am),
                   wasm_v128_or(wasm_i8x16_eq(v, v_lt),
                   wasm_v128_or(wasm_i8x16_eq(v, v_gt),
                   wasm_v128_or(wasm_i8x16_eq(v, v_qt), wasm_i8x16_eq(v, v_ap)))))))));
        if (json_esc) m = wasm_v128_or(m, wasm_u8x16_lt(v, v_sp));
        int32_t mask = wasm_i8x16_bitmask(m);
        if (mask == 0) {
          if (out->n + 16 > out->cap) buf_grow(out, 16);
          wasm_v128_store(out->d + out->n, v);
          out->n += 16;
          i += 16;
          last = i;
        } else {
          uint32_t tz = (uint32_t)__builtin_ctz((unsigned)mask);
          if (tz) { buf_put(out, s + i, tz); i += tz; last = i; }
          break;
        }
      }
      if (i >= end) break;
    }

    uint8_t c = (uint8_t)s[i];
    if (c >= 128 || tbl[c] == 0) { i++; continue; }
    if (i > last) buf_put(out, s + last, i - last);
    if (c == '&') { buf_put(out, "&amp;", 5); i++; last = i; continue; }
    if (c == '<') { buf_put(out, "&lt;", 4); i++; last = i; continue; }
    if (c == '>') { buf_put(out, "&gt;", 4); i++; last = i; continue; }
    if (c == '"') { buf_put(out, "&quot;", 6); i++; last = i; continue; }
    if (c == '\'') { buf_put(out, "&#39;", 5); i++; last = i; continue; }
    if (c < 32) { buf_put_ctrl(out, c); i++; last = i; continue; }

    if (c == '\\') {
      if (i + 1 < end && (uint8_t)s[i + 1] < 128 && ESC_TBL[(uint8_t)s[i + 1]]) {
        char n = s[i + 1];
        if (n == '>') buf_put(out, "&gt;", 4);
        else if (n == '\\') buf_put(out, json_esc ? "\\\\" : "\\", json_esc ? 2 : 1);
        else buf_putc(out, n);
        i += 2; last = i; continue;
      }
      buf_put(out, json_esc ? "\\\\" : "\\", json_esc ? 2 : 1);
      i++; last = i; continue;
    }

    if (c == '`') {
      int32_t j = -1;
      for (uint32_t k = i + 1; k < end; k++) { if (s[k] == '`') { j = (int32_t)k; break; } }
      if (j > (int32_t)(i + 1)) {
        buf_put(out, "<code>", 6);
        if (json_esc) buf_put_html_json(out, s + i + 1, (uint32_t)j - (i + 1));
        else buf_put_html(out, s + i + 1, (uint32_t)j - (i + 1));
        buf_put(out, "</code>", 7);
        i = (uint32_t)j + 1;
      } else {
        buf_putc(out, '`'); i++;
      }
      last = i; continue;
    }

    if (c == '*') {
      if (i + 1 < end && s[i + 1] == '*') {
        int32_t j = find_unesc_dstar(s, i + 2, end);
        if (j > (int32_t)(i + 2)) {
          buf_put(out, "<strong>", 8);
          render_inline(out, s, i + 2, (uint32_t)j, lc, json_esc);
          buf_put(out, "</strong>", 9);
          i = (uint32_t)j + 2;
        } else {
          buf_put(out, "**", 2); i += 2;
        }
      } else {
        int32_t j = find_single_star(s, i + 1, end);
        if (j > (int32_t)(i + 1)) {
          buf_put(out, "<em>", 4);
          render_inline(out, s, i + 1, (uint32_t)j, lc, json_esc);
          buf_put(out, "</em>", 5);
          i = (uint32_t)j + 1;
        } else {
          buf_putc(out, '*'); i++;
        }
      }
      last = i; continue;
    }

    if (c == '[') {
      int32_t k = find_unesc_char(s, ']', i + 1, end);
      int32_t m = -1;
      if (k != -1 && (uint32_t)k + 1 < end && s[k + 1] == '(') {
        for (uint32_t q = (uint32_t)k + 2; q < end; q++) { if (s[q] == ')') { m = (int32_t)q; break; } }
      }
      if (m > k + 2 && !has_ws_range(s, (uint32_t)k + 2, (uint32_t)m)) {
        Str target = str_lit(s + k + 2, (uint32_t)(m - (k + 2)));
        Str href = resolve_link(lc, target);
        if (json_esc) {
          buf_put(out, "<a href=\\\"", 10);
          buf_put_html_json(out, href.p, href.n);
          buf_put(out, "\\\">", 3);
        } else {
          buf_put(out, "<a href=\"", 9);
          buf_put_html(out, href.p, href.n);
          buf_put(out, "\">", 2);
        }
        render_inline(out, s, i + 1, (uint32_t)k, lc, json_esc);
        buf_put(out, "</a>", 4);
        i = (uint32_t)m + 1;
      } else {
        buf_putc(out, '['); i++;
      }
      last = i; continue;
    }
  }
  if (last < end) buf_put(out, s + last, end - last);
}

static bool is_blank(const char *s, uint32_t a, uint32_t b) {
  for (uint32_t i = a; i < b; i++) if (s[i] != ' ' && s[i] != '\t') return false;
  return true;
}

static Str trim_slice(const char *s, uint32_t a, uint32_t b) {
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return str_lit(s + a, b - a);
}

static bool match_fence_open(const char *s, uint32_t a, uint32_t b, Str *lang) {
  if (b - a < 3 || s[a] != '`' || s[a + 1] != '`' || s[a + 2] != '`') return false;
  uint32_t p = a + 3;
  while (p < b) {
    char c = s[p];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') p++;
    else break;
  }
  uint32_t le = p;
  while (p < b) { if (s[p] != ' ' && s[p] != '\t') return false; p++; }
  *lang = str_lit(s + a + 3, le - (a + 3));
  return true;
}

static bool is_fence_close(const char *s, uint32_t a, uint32_t b) {
  if (b - a < 3 || s[a] != '`' || s[a + 1] != '`' || s[a + 2] != '`') return false;
  for (uint32_t p = a + 3; p < b; p++) if (s[p] != ' ' && s[p] != '\t') return false;
  return true;
}

static int match_heading(const char *s, uint32_t a, uint32_t b, uint32_t *hs, uint32_t *he) {
  if (a >= b || s[a] != '#') return 0;
  uint32_t p = a;
  while (p < b && s[p] == '#' && p - a < 7) p++;
  int depth = (int)(p - a);
  if (depth < 1 || depth > 6 || p >= b || (s[p] != ' ' && s[p] != '\t')) return 0;
  while (p < b && (s[p] == ' ' || s[p] == '\t')) p++;
  uint32_t e = b;
  while (e > p && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
  if (e <= p) return 0;
  *hs = p; *he = e;
  return depth;
}

static bool is_hr(const char *s, uint32_t a, uint32_t b) {
  if (b - a < 3) return false;
  char c0 = s[a];
  if (c0 != '-' && c0 != '*') return false;
  if (s[a + 1] != c0 || s[a + 2] != c0) return false;
  for (uint32_t p = a + 3; p < b; p++) if (s[p] != ' ' && s[p] != '\t') return false;
  return true;
}

static int32_t ordered_start(const char *s, uint32_t a, uint32_t b, uint32_t *num) {
  uint32_t p = a, v = 0;
  while (p < b && p - a < 9 && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (uint32_t)(s[p++] - '0'); }
  if (p == a || p + 1 >= b) return -1;
  if (s[p] == '.' && s[p + 1] == ' ') { *num = v; return (int32_t)(p + 2); }
  return -1;
}

static bool is_continued(const char *s, uint32_t a, uint32_t b) {
  if (b - a < 3 || s[a] != ' ' || s[a + 1] != ' ') return false;
  uint32_t p = a + 2;
  while (p < b && s[p] == ' ') p++;
  if (p >= b) return false;
  uint8_t c = (uint8_t)s[p];
  return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
}

static inline uint32_t next_nl(const char *s, uint32_t pos, uint32_t len) {
  v128_t v_nl = wasm_i8x16_splat('\n');
  while (pos + 16 <= len) {
    int32_t m = wasm_i8x16_bitmask(wasm_i8x16_eq(wasm_v128_load(s + pos), v_nl));
    if (m == 0) pos += 16;
    else return pos + (uint32_t)__builtin_ctz((unsigned)m);
  }
  while (pos < len && s[pos] != '\n') pos++;
  return pos;
}

static void render_markdown(SourceDoc *doc, LinkCtx *lc, StrMap *slug_cache, bool json_esc) {
  const char *src = doc->body.p;
  uint32_t len = doc->body.n;
  StrMap ids; map_init(&ids, 16);
  TocItem *toc = (TocItem *)arena_alloc(32 * sizeof(TocItem));
  uint32_t n_toc = 0, cap_toc = 32;
  bool has_excerpt = false;
  Str excerpt = str_lit("", 0);

  Buf html; buf_init(&html, len + len / 2 + 256);
  Buf tmp; buf_init(&tmp, 512);
  Str NL = json_esc ? S("\\n") : S("\n");
  Str Q = json_esc ? S("\\\"") : S("\"");

  uint32_t pos = 0;
  while (pos < len) {
    uint32_t end = next_nl(src, pos, len);
    uint32_t npos = end < len ? end + 1 : len;
    if (is_blank(src, pos, end)) { pos = npos; continue; }

    char c0 = src[pos];
    Str lang;
    if (c0 == '`' && match_fence_open(src, pos, end, &lang)) {
      if (lang.n == 0) buf_puts(&html, S("<pre><code>"));
      else {
        buf_puts(&html, S("<pre><code class=")); buf_puts(&html, Q);
        buf_puts(&html, S("language-")); buf_puts(&html, lang);
        buf_puts(&html, Q); buf_putc(&html, '>');
      }
      pos = npos;
      while (pos < len) {
        end = next_nl(src, pos, len);
        npos = end < len ? end + 1 : len;
        if (is_fence_close(src, pos, end)) { pos = npos; break; }
        if (json_esc) buf_put_html_json(&html, src + pos, end - pos);
        else buf_put_html(&html, src + pos, end - pos);
        buf_puts(&html, NL);
        pos = npos;
      }
      buf_puts(&html, S("</code></pre>")); buf_puts(&html, NL);
      continue;
    }

    uint32_t hs = 0, he = 0;
    int depth;
    if (c0 == '#' && (depth = match_heading(src, pos, end, &hs, &he)) > 0) {
      Str htext = str_lit(src + hs, he - hs);
      Str base_slug = slugify_cached(htext, slug_cache);
      Str id = unique_slug(base_slug, &ids);
      buf_puts(&html, S("<h")); buf_putc(&html, (char)('0' + depth));
      buf_puts(&html, S(" id=")); buf_puts(&html, Q); buf_puts(&html, id); buf_puts(&html, Q); buf_putc(&html, '>');
      uint32_t h_start = html.n;
      render_inline(&html, src, hs, he, lc, json_esc);
      uint32_t h_len = html.n - h_start;
      if (depth == 2 || depth == 3) {
        char *hcopy = (char *)arena_alloc(h_len);
        memcpy(hcopy, html.d + h_start, h_len);
        if (n_toc == cap_toc) {
          cap_toc *= 2;
          TocItem *nt = (TocItem *)arena_alloc(cap_toc * sizeof(TocItem));
          memcpy(nt, toc, n_toc * sizeof(TocItem));
          toc = nt;
        }
        toc[n_toc].level = depth;
        toc[n_toc].id = id;
        toc[n_toc].html = str_lit(hcopy, h_len);
        n_toc++;
      }
      buf_puts(&html, S("</h")); buf_putc(&html, (char)('0' + depth)); buf_putc(&html, '>'); buf_puts(&html, NL);
      pos = npos;
      continue;
    }

    if ((c0 == '-' || c0 == '*') && is_hr(src, pos, end)) {
      buf_puts(&html, S("<hr>")); buf_puts(&html, NL);
      pos = npos;
      continue;
    }

    if (c0 == '>') {
      tmp.n = 0;
      while (pos < len) {
        end = next_nl(src, pos, len);
        npos = end < len ? end + 1 : len;
        if (pos >= end || src[pos] != '>') break;
        uint32_t qs = pos + 1;
        if (qs < end && src[qs] == ' ') qs++;
        Str tr = trim_slice(src, qs, end);
        if (tr.n > 0) {
          if (tmp.n > 0) buf_putc(&tmp, '\n');
          buf_puts(&tmp, tr);
        }
        pos = npos;
      }
      if (tmp.n == 0) {
        buf_puts(&html, S("<blockquote>")); buf_puts(&html, NL);
        buf_puts(&html, S("</blockquote>")); buf_puts(&html, NL);
      } else {
        buf_puts(&html, S("<blockquote>")); buf_puts(&html, NL);
        buf_puts(&html, S("<p>"));
        render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
        buf_puts(&html, S("</p>")); buf_puts(&html, NL);
        buf_puts(&html, S("</blockquote>")); buf_puts(&html, NL);
      }
      continue;
    }

    if ((c0 == '-' || c0 == '*') && pos + 1 < end && src[pos + 1] == ' ') {
      buf_puts(&html, S("<ul>")); buf_puts(&html, NL);
      tmp.n = 0;
      buf_puts(&tmp, trim_slice(src, pos + 2, end));
      pos = npos;
      while (pos < len) {
        end = next_nl(src, pos, len);
        npos = end < len ? end + 1 : len;
        if ((src[pos] == '-' || src[pos] == '*') && pos + 1 < end && src[pos + 1] == ' ') {
          buf_puts(&html, S("<li>"));
          render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
          buf_puts(&html, S("</li>")); buf_puts(&html, NL);
          tmp.n = 0;
          buf_puts(&tmp, trim_slice(src, pos + 2, end));
          pos = npos;
        } else if (is_continued(src, pos, end)) {
          buf_putc(&tmp, '\n');
          buf_puts(&tmp, trim_slice(src, pos, end));
          pos = npos;
        } else break;
      }
      buf_puts(&html, S("<li>"));
      render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
      buf_puts(&html, S("</li>")); buf_puts(&html, NL);
      buf_puts(&html, S("</ul>")); buf_puts(&html, NL);
      continue;
    }

    uint32_t snum = 0;
    int32_t rs;
    if (c0 >= '0' && c0 <= '9' && (rs = ordered_start(src, pos, end, &snum)) != -1) {
      if (snum != 1) {
        buf_puts(&html, S("<ol start=")); buf_puts(&html, Q); buf_put_u32(&html, snum); buf_puts(&html, Q); buf_putc(&html, '>'); buf_puts(&html, NL);
      } else {
        buf_puts(&html, S("<ol>")); buf_puts(&html, NL);
      }
      tmp.n = 0;
      buf_puts(&tmp, trim_slice(src, (uint32_t)rs, end));
      pos = npos;
      while (pos < len) {
        end = next_nl(src, pos, len);
        npos = end < len ? end + 1 : len;
        uint32_t dummy = 0;
        int32_t r2 = (pos < end && src[pos] >= '0' && src[pos] <= '9') ? ordered_start(src, pos, end, &dummy) : -1;
        if (r2 != -1) {
          buf_puts(&html, S("<li>"));
          render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
          buf_puts(&html, S("</li>")); buf_puts(&html, NL);
          tmp.n = 0;
          buf_puts(&tmp, trim_slice(src, (uint32_t)r2, end));
          pos = npos;
        } else if (is_continued(src, pos, end)) {
          buf_putc(&tmp, '\n');
          buf_puts(&tmp, trim_slice(src, pos, end));
          pos = npos;
        } else break;
      }
      buf_puts(&html, S("<li>"));
      render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
      buf_puts(&html, S("</li>")); buf_puts(&html, NL);
      buf_puts(&html, S("</ol>")); buf_puts(&html, NL);
      continue;
    }

    tmp.n = 0;
    buf_puts(&tmp, trim_slice(src, pos, end));
    pos = npos;
    while (pos < len) {
      end = next_nl(src, pos, len);
      npos = end < len ? end + 1 : len;
      if (is_blank(src, pos, end) ||
          (src[pos] == '`' && match_fence_open(src, pos, end, &lang)) ||
          (src[pos] == '#' && match_heading(src, pos, end, &hs, &he) > 0) ||
          ((src[pos] == '-' || src[pos] == '*') && is_hr(src, pos, end))) break;
      buf_putc(&tmp, '\n');
      buf_puts(&tmp, trim_slice(src, pos, end));
      pos = npos;
    }
    buf_puts(&html, S("<p>"));
    uint32_t p_start = html.n;
    render_inline(&html, tmp.d, 0, tmp.n, lc, json_esc);
    if (!has_excerpt) {
      uint32_t plen = html.n - p_start;
      char *ecopy = (char *)arena_alloc(plen);
      memcpy(ecopy, html.d + p_start, plen);
      excerpt = str_lit(ecopy, plen);
      has_excerpt = true;
    }
    buf_puts(&html, S("</p>")); buf_puts(&html, NL);
  }

  doc->html = str_lit(html.d, html.n);
  doc->toc = toc;
  doc->n_toc = n_toc;
  doc->excerpt = excerpt;
}

// ---------------- Templates (Enum PropId + Constant Folding + Pre-JSON-Escape) ----------------
enum { NK_TEXT, NK_TEXT_PREESC, NK_VAR, NK_RAW, NK_IF, NK_FOR, NK_INCLUDE };

enum {
  P_UNKNOWN = 0,
  P_TITLE, P_BASE_URL, P_NAME, P_SLUG, P_URL, P_COUNT, P_POSTS,
  P_DATE, P_DATE_LONG, P_SUMMARY, P_EXCERPT, P_READING_MINUTES, P_TAGS,
  P_CONTENT, P_TOC, P_IS_POST, P_NEWER, P_OLDER,
  P_LEVEL, P_ID, P_HTML,
  P_NUMBER, P_TOTAL, P_NEWER_URL, P_OLDER_URL,
  P_INDEX, P_FIRST, P_LAST, P_LENGTH
};

static uint8_t prop_id_of(Str k) {
  switch (k.n) {
    case 2:
      if (str_eq(k, S("id"))) return P_ID;
      break;
    case 3:
      if (str_eq(k, S("url"))) return P_URL;
      if (str_eq(k, S("toc"))) return P_TOC;
      break;
    case 4:
      if (str_eq(k, S("name"))) return P_NAME;
      if (str_eq(k, S("slug"))) return P_SLUG;
      if (str_eq(k, S("date"))) return P_DATE;
      if (str_eq(k, S("tags"))) return P_TAGS;
      if (str_eq(k, S("html"))) return P_HTML;
      if (str_eq(k, S("last"))) return P_LAST;
      break;
    case 5:
      if (str_eq(k, S("title"))) return P_TITLE;
      if (str_eq(k, S("count"))) return P_COUNT;
      if (str_eq(k, S("posts"))) return P_POSTS;
      if (str_eq(k, S("newer"))) return P_NEWER;
      if (str_eq(k, S("older"))) return P_OLDER;
      if (str_eq(k, S("level"))) return P_LEVEL;
      if (str_eq(k, S("total"))) return P_TOTAL;
      if (str_eq(k, S("index"))) return P_INDEX;
      if (str_eq(k, S("first"))) return P_FIRST;
      break;
    case 6:
      if (str_eq(k, S("number"))) return P_NUMBER;
      if (str_eq(k, S("length"))) return P_LENGTH;
      break;
    case 7:
      if (str_eq(k, S("summary"))) return P_SUMMARY;
      if (str_eq(k, S("excerpt"))) return P_EXCERPT;
      if (str_eq(k, S("content"))) return P_CONTENT;
      if (str_eq(k, S("is_post"))) return P_IS_POST;
      break;
    case 8:
      if (str_eq(k, S("base_url"))) return P_BASE_URL;
      break;
    case 9:
      if (str_eq(k, S("date_long"))) return P_DATE_LONG;
      if (str_eq(k, S("newer_url"))) return P_NEWER_URL;
      if (str_eq(k, S("older_url"))) return P_OLDER_URL;
      break;
    case 15:
      if (str_eq(k, S("reading_minutes"))) return P_READING_MINUTES;
      break;
  }
  return P_UNKNOWN;
}

typedef struct TNode TNode;
typedef struct { TNode **items; uint32_t n; uint32_t cap; } NodeList;

struct TNode {
  uint8_t kind;
  bool is_not;
  bool is_md_field;
  Str text; // text or for-var or include-name
  Str *path; uint32_t n_path;
  uint8_t *pids;
  NodeList body;
  NodeList else_body;
  bool has_else;
};

static void nl_push(NodeList *l, TNode *n) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    TNode **ni = (TNode **)arena_alloc(l->cap * sizeof(TNode *));
    if (l->n) memcpy(ni, l->items, l->n * sizeof(TNode *));
    l->items = ni;
  }
  l->items[l->n++] = n;
}

static bool is_ident(Str s) {
  if (s.n == 0) return false;
  char c0 = s.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return false;
  for (uint32_t i = 1; i < s.n; i++) {
    char c = s.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

static bool parse_tpath(Str tpl_name, Str text, Str **out_p, uint8_t **out_ids, uint32_t *out_n, Str *err) {
  uint32_t cnt = 1;
  for (uint32_t i = 0; i < text.n; i++) if (text.p[i] == '.') cnt++;
  Str *parts = (Str *)arena_alloc(cnt * sizeof(Str));
  uint8_t *ids = (uint8_t *)arena_alloc(cnt * sizeof(uint8_t));
  uint32_t s = 0, pi = 0;
  for (uint32_t i = 0; i <= text.n; i++) {
    if (i == text.n || text.p[i] == '.') {
      Str seg = str_lit(text.p + s, i - s);
      if (!is_ident(seg)) {
        Buf eb; buf_init(&eb, tpl_name.n + text.n + 40);
        buf_puts(&eb, S("template \"")); buf_puts(&eb, tpl_name); buf_puts(&eb, S("\": \""));
        buf_put_json(&eb, text.p, text.n); buf_puts(&eb, S("\" is not a path"));
        *err = str_lit(eb.d, eb.n);
        return false;
      }
      parts[pi] = seg;
      ids[pi] = prop_id_of(seg);
      pi++;
      s = i + 1;
    }
  }
  *out_p = parts; *out_ids = ids; *out_n = pi;
  return true;
}

static bool is_md_pid(uint8_t pid) {
  return pid == P_CONTENT || pid == P_EXCERPT || pid == P_HTML;
}

static bool parse_template(Str name, Str src, NodeList *out_root, bool *uses_esc_md, Str *err) {
  TNode *root = (TNode *)arena_calloc(sizeof(TNode));
  TNode *stack[64];
  uint32_t sp = 0;
  stack[sp++] = root;
  NodeList *cur = &root->body;

  Buf txt; buf_init(&txt, 256);
  uint32_t i = 0, len = src.n;

  while (i < len) {
    int32_t open = -1;
    for (uint32_t k = i; k < len; k++) { if (src.p[k] == '{') { open = (int32_t)k; break; } }
    if (open == -1 || (uint32_t)open == len - 1) {
      buf_put(&txt, src.p + i, len - i);
      break;
    }
    char nxt = src.p[open + 1];
    if (nxt != '{' && nxt != '%') {
      buf_put(&txt, src.p + i, (uint32_t)open + 1 - i);
      i = (uint32_t)open + 1;
      continue;
    }
    buf_put(&txt, src.p + i, (uint32_t)open - i);
    if (nxt == '{') {
      bool raw = ((uint32_t)open + 2 < len && src.p[open + 2] == '{');
      uint32_t dlen = raw ? 3 : 2;
      int32_t close = -1;
      for (uint32_t k = (uint32_t)open + dlen; k + dlen <= len; k++) {
        if (src.p[k] == '}' && src.p[k + 1] == '}' && (!raw || src.p[k + 2] == '}')) { close = (int32_t)k; break; }
      }
      if (close == -1) {
        *err = fmt_err2(S("template \""), name, raw ? S("\": a {{{ is not closed") : S("\": a {{ is not closed"));
        return false;
      }
      if (txt.n > 0) {
        TNode *tn = (TNode *)arena_calloc(sizeof(TNode));
        tn->kind = NK_TEXT;
        char *cp = (char *)arena_alloc(txt.n); memcpy(cp, txt.d, txt.n);
        tn->text = str_lit(cp, txt.n);
        nl_push(cur, tn);
        txt.n = 0;
      }
      Str inner = trim_slice(src.p, (uint32_t)open + dlen, (uint32_t)close);
      TNode *vn = (TNode *)arena_calloc(sizeof(TNode));
      vn->kind = raw ? NK_RAW : NK_VAR;
      if (!parse_tpath(name, inner, &vn->path, &vn->pids, &vn->n_path, err)) return false;
      vn->is_md_field = is_md_pid(vn->pids[vn->n_path - 1]);
      if (!raw && vn->is_md_field) *uses_esc_md = true;
      nl_push(cur, vn);
      i = (uint32_t)close + dlen;
      continue;
    }
    int32_t close = -1;
    for (uint32_t k = (uint32_t)open + 2; k + 2 <= len; k++) {
      if (src.p[k] == '%' && src.p[k + 1] == '}') { close = (int32_t)k; break; }
    }
    if (close == -1) {
      *err = fmt_err2(S("template \""), name, S("\": a {% is not closed"));
      return false;
    }
    uint32_t end = (uint32_t)close + 2;
    uint32_t ls = (uint32_t)open;
    while (ls > 0 && src.p[ls - 1] != '\n') ls--;
    uint32_t le = end;
    while (le < len && src.p[le] != '\n') le++;
    bool standalone = true;
    for (uint32_t k = ls; k < (uint32_t)open; k++) if (src.p[k] != ' ' && src.p[k] != '\t') { standalone = false; break; }
    if (standalone) for (uint32_t k = end; k < le; k++) if (src.p[k] != ' ' && src.p[k] != '\t') { standalone = false; break; }
    if (standalone) txt.n -= ((uint32_t)open - ls);
    if (txt.n > 0) {
      TNode *tn = (TNode *)arena_calloc(sizeof(TNode));
      tn->kind = NK_TEXT;
      char *cp = (char *)arena_alloc(txt.n); memcpy(cp, txt.d, txt.n);
      tn->text = str_lit(cp, txt.n);
      nl_push(cur, tn);
      txt.n = 0;
    }
    Str tag = trim_slice(src.p, (uint32_t)open + 2, (uint32_t)close);
    i = standalone ? (le < len ? le + 1 : len) : end;

    TNode *top = stack[sp - 1];
    if (str_starts(tag, S("for "))) {
      uint32_t p1 = 4;
      while (p1 < tag.n && tag.p[p1] != ' ') p1++;
      Str vname = str_lit(tag.p + 4, p1 - 4);
      if (!is_ident(vname) || p1 + 4 > tag.n || my_memcmp(tag.p + p1, " in ", 4) != 0) {
        *err = fmt_err2(S("template \""), name, fmt_err2(S("\": {% "), tag, S(" %} is not a tag")));
        return false;
      }
      Str rest = str_lit(tag.p + p1 + 4, tag.n - (p1 + 4));
      if (has_ws_range(rest.p, 0, rest.n)) {
        *err = fmt_err2(S("template \""), name, fmt_err2(S("\": {% "), tag, S(" %} is not a tag")));
        return false;
      }
      TNode *fn = (TNode *)arena_calloc(sizeof(TNode));
      fn->kind = NK_FOR;
      fn->text = vname;
      if (!parse_tpath(name, rest, &fn->path, &fn->pids, &fn->n_path, err)) return false;
      nl_push(cur, fn);
      stack[sp++] = fn;
      cur = &fn->body;
    } else if (str_starts(tag, S("if "))) {
      Str rest = str_lit(tag.p + 3, tag.n - 3);
      bool is_not = false;
      if (str_starts(rest, S("not "))) { is_not = true; rest = str_lit(rest.p + 4, rest.n - 4); }
      if (rest.n == 0 || has_ws_range(rest.p, 0, rest.n)) {
        *err = fmt_err2(S("template \""), name, fmt_err2(S("\": {% "), tag, S(" %} is not a tag")));
        return false;
      }
      TNode *in = (TNode *)arena_calloc(sizeof(TNode));
      in->kind = NK_IF;
      in->is_not = is_not;
      if (!parse_tpath(name, rest, &in->path, &in->pids, &in->n_path, err)) return false;
      nl_push(cur, in);
      stack[sp++] = in;
      cur = &in->body;
    } else if (str_eq(tag, S("else"))) {
      if (sp <= 1 || top->kind != NK_IF || top->has_else) {
        *err = fmt_err2(S("template \""), name, S("\": an else outside an if"));
        return false;
      }
      top->has_else = true;
      cur = &top->else_body;
    } else if (str_eq(tag, S("endfor")) || str_eq(tag, S("endif"))) {
      bool is_for = str_eq(tag, S("endfor"));
      if (sp <= 1 || top->kind != (is_for ? NK_FOR : NK_IF)) {
        *err = fmt_err2(S("template \""), name, is_for ? S("\": an endfor with no open for") : S("\": an endif with no open if"));
        return false;
      }
      sp--;
      TNode *par = stack[sp - 1];
      cur = (sp == 1 || par->kind == NK_FOR) ? &par->body : (par->has_else ? &par->else_body : &par->body);
    } else if (str_starts(tag, S("include \"")) && tag.n > 10 && tag.p[tag.n - 1] == '"') {
      Str inc = str_lit(tag.p + 9, tag.n - 10);
      if (!is_name_ok(inc)) {
        *err = fmt_err2(S("template \""), name, fmt_err2(S("\": {% "), tag, S(" %} is not a tag")));
        return false;
      }
      TNode *in = (TNode *)arena_calloc(sizeof(TNode));
      in->kind = NK_INCLUDE;
      in->text = inc;
      nl_push(cur, in);
    } else {
      *err = fmt_err2(S("template \""), name, fmt_err2(S("\": {% "), tag, S(" %} is not a tag")));
      return false;
    }
  }
  if (txt.n > 0) {
    TNode *tn = (TNode *)arena_calloc(sizeof(TNode));
    tn->kind = NK_TEXT;
    char *cp = (char *)arena_alloc(txt.n); memcpy(cp, txt.d, txt.n);
    tn->text = str_lit(cp, txt.n);
    nl_push(cur, tn);
  }
  if (sp > 1) {
    *err = fmt_err2(S("template \""), name, stack[sp - 1]->kind == NK_FOR ? S("\": a for is not closed") : S("\": a if is not closed"));
    return false;
  }
  *out_root = root->body;
  return true;
}

// ---------------- Template Runtime Value (O(1) Enum Dispatch) ----------------
enum {
  VT_UNDEF, VT_NULL, VT_BOOL, VT_INT, VT_STR, VT_ARR,
  VT_SITE, VT_TAG, VT_CARD, VT_PAGE, VT_LINK, VT_TOC, VT_PAGER, VT_LOOP
};

typedef struct Val Val;
typedef struct { int32_t number, total; Str url, newer_url, older_url; } PagerObj;
typedef struct { int32_t index, length; bool first, last; } LoopObj;

struct Val {
  uint8_t t;
  bool b;
  int32_t i;
  Str s;
  void *ptr;
  uint32_t len;
};

static inline Val v_undef(void) { Val v = {0}; return v; }
static inline Val v_str(Str s) { Val v = {0}; v.t = VT_STR; v.s = s; return v; }
static inline Val v_int(int32_t x) { Val v = {0}; v.t = VT_INT; v.i = x; return v; }
static inline Val v_bool(bool b) { Val v = {0}; v.t = VT_BOOL; v.b = b; return v; }
static inline Val v_arr(Val *items, uint32_t n) { Val v = {0}; v.t = VT_ARR; v.ptr = items; v.len = n; return v; }

static inline bool val_truthy(Val v) {
  switch (v.t) {
    case VT_UNDEF: case VT_NULL: return false;
    case VT_BOOL: return v.b;
    case VT_INT: return v.i != 0;
    case VT_STR: return v.s.n > 0;
    case VT_ARR: return v.len > 0;
    default: return true;
  }
}

typedef struct { Str title, base_url; } SiteObj;
typedef struct { SourceDoc *doc; Val tags_arr; } CardObj;
typedef struct { SourceDoc *doc; CardObj *card; Val toc_arr; } PageObj;
typedef struct { TagObj *tag; bool include_posts; Val posts_arr; } TagValObj;

static inline Val get_prop(Val v, uint8_t pid) {
  switch (v.t) {
    case VT_SITE: {
      SiteObj *o = (SiteObj *)v.ptr;
      if (pid == P_TITLE) return v_str(o->title);
      if (pid == P_BASE_URL) return v_str(o->base_url);
      return v_undef();
    }
    case VT_TAG: {
      TagValObj *o = (TagValObj *)v.ptr;
      switch (pid) {
        case P_NAME: return v_str(o->tag->name);
        case P_SLUG: return v_str(o->tag->slug);
        case P_URL: return v_str(o->tag->url);
        case P_COUNT: return v_int((int32_t)o->tag->n_posts);
        case P_POSTS: return o->include_posts ? o->posts_arr : v_undef();
        default: return v_undef();
      }
    }
    case VT_CARD: {
      CardObj *co = (CardObj *)v.ptr;
      SourceDoc *d = co->doc;
      switch (pid) {
        case P_TITLE: return v_str(d->fm.title);
        case P_URL: return v_str(d->url);
        case P_DATE: return v_str(d->fm.has_date ? d->fm.date : str_lit("", 0));
        case P_DATE_LONG: return v_str(d->date_long);
        case P_SUMMARY: return v_str(d->fm.has_summary ? d->fm.summary : str_lit("", 0));
        case P_EXCERPT: return v_str(d->excerpt);
        case P_READING_MINUTES: return v_int((int32_t)d->reading_minutes);
        case P_TAGS: return co->tags_arr;
        default: return v_undef();
      }
    }
    case VT_PAGE: {
      PageObj *po = (PageObj *)v.ptr;
      SourceDoc *d = po->doc;
      switch (pid) {
        case P_TITLE: return v_str(d->fm.title);
        case P_URL: return v_str(d->url);
        case P_DATE: return v_str(d->fm.has_date ? d->fm.date : str_lit("", 0));
        case P_DATE_LONG: return v_str(d->date_long);
        case P_SUMMARY: return v_str(d->fm.has_summary ? d->fm.summary : str_lit("", 0));
        case P_EXCERPT: return v_str(d->excerpt);
        case P_READING_MINUTES: return v_int((int32_t)d->reading_minutes);
        case P_TAGS: return po->card->tags_arr;
        case P_SLUG: return v_str(d->slug);
        case P_CONTENT: return v_str(d->html);
        case P_TOC: return po->toc_arr;
        case P_IS_POST: return v_bool(d->is_post);
        case P_NEWER: { Val r = {0}; r.t = d->newer ? VT_LINK : VT_NULL; r.ptr = d->newer; return r; }
        case P_OLDER: { Val r = {0}; r.t = d->older ? VT_LINK : VT_NULL; r.ptr = d->older; return r; }
        default: return v_undef();
      }
    }
    case VT_LINK: {
      SourceDoc *d = (SourceDoc *)v.ptr;
      switch (pid) {
        case P_TITLE: return v_str(d->fm.title);
        case P_URL: return v_str(d->url);
        case P_DATE: return v_str(d->fm.has_date ? d->fm.date : str_lit("", 0));
        case P_DATE_LONG: return v_str(d->date_long);
        default: return v_undef();
      }
    }
    case VT_TOC: {
      TocItem *t = (TocItem *)v.ptr;
      if (pid == P_LEVEL) return v_int(t->level);
      if (pid == P_ID) return v_str(t->id);
      if (pid == P_HTML) return v_str(t->html);
      return v_undef();
    }
    case VT_PAGER: {
      PagerObj *p = (PagerObj *)v.ptr;
      switch (pid) {
        case P_NUMBER: return v_int(p->number);
        case P_TOTAL: return v_int(p->total);
        case P_URL: return v_str(p->url);
        case P_NEWER_URL: return v_str(p->newer_url);
        case P_OLDER_URL: return v_str(p->older_url);
        default: return v_undef();
      }
    }
    case VT_LOOP: {
      LoopObj *l = (LoopObj *)v.ptr;
      if (pid == P_INDEX) return v_int(l->index);
      if (pid == P_FIRST) return v_bool(l->first);
      if (pid == P_LAST) return v_bool(l->last);
      if (pid == P_LENGTH) return v_int(l->length);
      return v_undef();
    }
    default: return v_undef();
  }
}

typedef struct { Str name; Val val; } Binding;

typedef struct {
  Binding binds[32];
  uint32_t n;
  StrMap *tpls;
  bool pre_esc_md;
} EvalCtx;

static inline Val eval_path(EvalCtx *ec, Str *path, uint8_t *pids, uint32_t n) {
  Val v = v_undef();
  bool found = false;
  Str p0 = path[0];
  for (int32_t i = (int32_t)ec->n - 1; i >= 0; i--) {
    if (ec->binds[i].name.n == p0.n && my_memcmp(ec->binds[i].name.p, p0.p, p0.n) == 0) {
      v = ec->binds[i].val; found = true; break;
    }
  }
  if (!found) return v_undef();
  for (uint32_t k = 1; k < n; k++) {
    v = get_prop(v, pids[k]);
    if (v.t == VT_UNDEF) return v;
  }
  return v;
}

static void emit_val_json(Buf *out, Val v, bool esc_html, bool raw_md) {
  if (v.t == VT_STR) {
    if (raw_md) buf_puts(out, v.s);
    else if (esc_html) buf_put_html_json(out, v.s.p, v.s.n);
    else buf_put_json(out, v.s.p, v.s.n);
  } else if (v.t == VT_INT) {
    buf_put_i32(out, v.i);
  } else if (v.t == VT_BOOL) {
    buf_puts(out, v.b ? S("true") : S("false"));
  } else if (v.t == VT_ARR) {
    Val *items = (Val *)v.ptr;
    for (uint32_t i = 0; i < v.len; i++) {
      if (i > 0) buf_put(out, ", ", 2);
      emit_val_json(out, items[i], esc_html, false);
    }
  }
}

static void eval_nodes(Buf *out, EvalCtx *ec, NodeList *list, int depth) {
  for (uint32_t i = 0; i < list->n; i++) {
    TNode *n = list->items[i];
    if (n->kind == NK_TEXT_PREESC) {
      buf_put(out, n->text.p, n->text.n);
    } else if (n->kind == NK_TEXT) {
      buf_put_json(out, n->text.p, n->text.n);
    } else if (n->kind == NK_VAR) {
      Val v = eval_path(ec, n->path, n->pids, n->n_path);
      emit_val_json(out, v, true, false);
    } else if (n->kind == NK_RAW) {
      Val v = eval_path(ec, n->path, n->pids, n->n_path);
      emit_val_json(out, v, false, ec->pre_esc_md && n->is_md_field && v.t == VT_STR);
    } else if (n->kind == NK_IF) {
      bool cond = val_truthy(eval_path(ec, n->path, n->pids, n->n_path));
      if (cond != n->is_not) eval_nodes(out, ec, &n->body, depth);
      else if (n->has_else) eval_nodes(out, ec, &n->else_body, depth);
    } else if (n->kind == NK_FOR) {
      Val arr = eval_path(ec, n->path, n->pids, n->n_path);
      if (arr.t == VT_ARR && arr.len > 0) {
        Val *items = (Val *)arr.ptr;
        uint32_t saved_n = ec->n;
        LoopObj lo;
        lo.length = (int32_t)arr.len;
        Val lval = {0}; lval.t = VT_LOOP; lval.ptr = &lo;
        ec->binds[ec->n++] = (Binding){ S("loop"), lval };
        uint32_t vidx = ec->n++;
        ec->binds[vidx].name = n->text;
        for (uint32_t k = 0; k < arr.len; k++) {
          lo.index = (int32_t)(k + 1);
          lo.first = (k == 0);
          lo.last = (k == arr.len - 1);
          ec->binds[vidx].val = items[k];
          eval_nodes(out, ec, &n->body, depth);
        }
        ec->n = saved_n;
      }
    } else if (n->kind == NK_INCLUDE) {
      if (depth < 8) {
        NodeList *inc = (NodeList *)map_get(ec->tpls, n->text);
        if (inc) eval_nodes(out, ec, inc, depth + 1);
      }
    }
  }
}

// ---------------- Constant Folding & Include Inlining ----------------
static bool in_str_arr(Str s, const Str *arr, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) if (str_eq(s, arr[i])) return true;
  return false;
}

static bool is_const_var(Str p0, const Str *dyn, uint32_t n_dyn, const Str *loc, uint32_t n_loc) {
  if (in_str_arr(p0, dyn, n_dyn)) return false;
  if (in_str_arr(p0, loc, n_loc)) return true;
  return str_eq(p0, S("site")) || str_eq(p0, S("tags")) || str_eq(p0, S("recent"));
}

static bool is_subtree_const(NodeList *list, StrMap *tpls, const Str *dyn, uint32_t n_dyn, Str *loc, uint32_t n_loc, int depth) {
  for (uint32_t i = 0; i < list->n; i++) {
    TNode *n = list->items[i];
    if (n->kind == NK_TEXT || n->kind == NK_TEXT_PREESC) continue;
    if (n->kind == NK_INCLUDE) {
      if (depth < 8) {
        NodeList *inc = (NodeList *)map_get(tpls, n->text);
        if (inc && !is_subtree_const(inc, tpls, dyn, n_dyn, loc, n_loc, depth + 1)) return false;
      }
      continue;
    }
    if (!is_const_var(n->path[0], dyn, n_dyn, loc, n_loc)) return false;
    if (n->kind == NK_IF) {
      if (!is_subtree_const(&n->body, tpls, dyn, n_dyn, loc, n_loc, depth)) return false;
      if (n->has_else && !is_subtree_const(&n->else_body, tpls, dyn, n_dyn, loc, n_loc, depth)) return false;
    } else if (n->kind == NK_FOR) {
      if (n_loc + 2 > 32) return false;
      loc[n_loc] = n->text;
      loc[n_loc + 1] = S("loop");
      if (!is_subtree_const(&n->body, tpls, dyn, n_dyn, loc, n_loc + 2, depth)) return false;
    }
  }
  return true;
}

static void flush_pending(NodeList *out_list, Buf *pending) {
  if (pending->n == 0) return;
  TNode *tn = (TNode *)arena_calloc(sizeof(TNode));
  tn->kind = NK_TEXT_PREESC;
  char *cp = (char *)arena_alloc(pending->n);
  memcpy(cp, pending->d, pending->n);
  tn->text = str_lit(cp, pending->n);
  nl_push(out_list, tn);
  pending->n = 0;
}

static void fold_nodes(NodeList *in_list, NodeList *out_list, Buf *pending, EvalCtx *const_ec, Str *dyn, uint32_t n_dyn, int depth) {
  for (uint32_t i = 0; i < in_list->n; i++) {
    TNode *n = in_list->items[i];
    if (n->kind == NK_TEXT) {
      buf_put_json(pending, n->text.p, n->text.n);
    } else if (n->kind == NK_INCLUDE) {
      if (depth < 8) {
        NodeList *inc = (NodeList *)map_get(const_ec->tpls, n->text);
        if (inc) fold_nodes(inc, out_list, pending, const_ec, dyn, n_dyn, depth + 1);
      }
    } else if (n->kind == NK_VAR) {
      if (is_const_var(n->path[0], dyn, n_dyn, NULL, 0)) {
        Val v = eval_path(const_ec, n->path, n->pids, n->n_path);
        emit_val_json(pending, v, true, false);
      } else {
        flush_pending(out_list, pending);
        nl_push(out_list, n);
      }
    } else if (n->kind == NK_RAW) {
      if (is_const_var(n->path[0], dyn, n_dyn, NULL, 0)) {
        Val v = eval_path(const_ec, n->path, n->pids, n->n_path);
        emit_val_json(pending, v, false, const_ec->pre_esc_md && n->is_md_field && v.t == VT_STR);
      } else {
        flush_pending(out_list, pending);
        nl_push(out_list, n);
      }
    } else if (n->kind == NK_IF) {
      if (is_const_var(n->path[0], dyn, n_dyn, NULL, 0)) {
        bool cond = val_truthy(eval_path(const_ec, n->path, n->pids, n->n_path));
        if (cond != n->is_not) fold_nodes(&n->body, out_list, pending, const_ec, dyn, n_dyn, depth);
        else if (n->has_else) fold_nodes(&n->else_body, out_list, pending, const_ec, dyn, n_dyn, depth);
      } else {
        flush_pending(out_list, pending);
        TNode *nf = (TNode *)arena_calloc(sizeof(TNode));
        nf->kind = NK_IF;
        nf->is_not = n->is_not;
        nf->path = n->path; nf->pids = n->pids; nf->n_path = n->n_path;
        nf->has_else = n->has_else;
        Buf sub_p; buf_init(&sub_p, 256);
        fold_nodes(&n->body, &nf->body, &sub_p, const_ec, dyn, n_dyn, depth);
        flush_pending(&nf->body, &sub_p);
        if (n->has_else) {
          sub_p.n = 0;
          fold_nodes(&n->else_body, &nf->else_body, &sub_p, const_ec, dyn, n_dyn, depth);
          flush_pending(&nf->else_body, &sub_p);
        }
        nl_push(out_list, nf);
      }
    } else if (n->kind == NK_FOR) {
      Str loc[32];
      loc[0] = n->text; loc[1] = S("loop");
      if (is_const_var(n->path[0], dyn, n_dyn, NULL, 0) &&
          is_subtree_const(&n->body, const_ec->tpls, dyn, n_dyn, loc, 2, depth)) {
        Val arr = eval_path(const_ec, n->path, n->pids, n->n_path);
        if (arr.t == VT_ARR && arr.len > 0) {
          Val *items = (Val *)arr.ptr;
          uint32_t saved_n = const_ec->n;
          LoopObj lo;
          lo.length = (int32_t)arr.len;
          Val lval = {0}; lval.t = VT_LOOP; lval.ptr = &lo;
          const_ec->binds[const_ec->n++] = (Binding){ S("loop"), lval };
          uint32_t vidx = const_ec->n++;
          const_ec->binds[vidx].name = n->text;
          for (uint32_t k = 0; k < arr.len; k++) {
            lo.index = (int32_t)(k + 1);
            lo.first = (k == 0);
            lo.last = (k == arr.len - 1);
            const_ec->binds[vidx].val = items[k];
            eval_nodes(pending, const_ec, &n->body, depth);
          }
          const_ec->n = saved_n;
        }
      } else {
        flush_pending(out_list, pending);
        TNode *nf = (TNode *)arena_calloc(sizeof(TNode));
        nf->kind = NK_FOR;
        nf->text = n->text;
        nf->path = n->path; nf->pids = n->pids; nf->n_path = n->n_path;
        dyn[n_dyn] = n->text;
        dyn[n_dyn + 1] = S("loop");
        Buf sub_p; buf_init(&sub_p, 256);
        fold_nodes(&n->body, &nf->body, &sub_p, const_ec, dyn, n_dyn + 2, depth);
        flush_pending(&nf->body, &sub_p);
        nl_push(out_list, nf);
      }
    }
  }
}

static NodeList *get_folded_tpl(Str name, StrMap *tpls, StrMap *folded_map, EvalCtx *const_ec) {
  NodeList *cached = (NodeList *)map_get(folded_map, name);
  if (cached) return cached;
  NodeList *raw = (NodeList *)map_get(tpls, name);
  if (!raw) return NULL;
  NodeList *fl = (NodeList *)arena_calloc(sizeof(NodeList));
  Buf pending; buf_init(&pending, 1024);
  Str dyn[32];
  fold_nodes(raw, fl, &pending, const_ec, dyn, 0, 0);
  flush_pending(fl, &pending);
  map_set(folded_map, name, fl);
  return fl;
}

// ---------------- Sorting (Merge Sort) ----------------
static void sort_tpls(InputTpl *a, InputTpl *tmp, uint32_t n) {
  if (n <= 1) return;
  uint32_t m = n / 2;
  sort_tpls(a, tmp, m);
  sort_tpls(a + m, tmp + m, n - m);
  uint32_t i = 0, j = m, k = 0;
  while (i < m && j < n) tmp[k++] = (str_cmp(a[i].name, a[j].name) <= 0) ? a[i++] : a[j++];
  while (i < m) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof(InputTpl));
}

static void sort_files(InputFile *a, InputFile *tmp, uint32_t n) {
  if (n <= 1) return;
  uint32_t m = n / 2;
  sort_files(a, tmp, m);
  sort_files(a + m, tmp + m, n - m);
  uint32_t i = 0, j = m, k = 0;
  while (i < m && j < n) tmp[k++] = (str_cmp(a[i].path, a[j].path) <= 0) ? a[i++] : a[j++];
  while (i < m) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof(InputFile));
}

static void sort_posts(SourceDoc **a, SourceDoc **tmp, uint32_t n) {
  if (n <= 1) return;
  uint32_t m = n / 2;
  sort_posts(a, tmp, m);
  sort_posts(a + m, tmp + m, n - m);
  uint32_t i = 0, j = m, k = 0;
  while (i < m && j < n) {
    int c = str_cmp(a[j]->fm.date, a[i]->fm.date);
    if (c == 0) c = str_cmp(a[i]->slug, a[j]->slug);
    tmp[k++] = (c <= 0) ? a[i++] : a[j++];
  }
  while (i < m) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof(SourceDoc *));
}

static void sort_tags(TagObj **a, TagObj **tmp, uint32_t n) {
  if (n <= 1) return;
  uint32_t m = n / 2;
  sort_tags(a, tmp, m);
  sort_tags(a + m, tmp + m, n - m);
  uint32_t i = 0, j = m, k = 0;
  while (i < m && j < n) {
    int c = (a[j]->n_posts > a[i]->n_posts) ? 1 : (a[j]->n_posts < a[i]->n_posts) ? -1 : str_cmp(a[i]->slug, a[j]->slug);
    tmp[k++] = (c <= 0) ? a[i++] : a[j++];
  }
  while (i < m) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof(TagObj *));
}

static void sort_strs(Str *a, Str *tmp, uint32_t n) {
  if (n <= 1) return;
  uint32_t m = n / 2;
  sort_strs(a, tmp, m);
  sort_strs(a + m, tmp + m, n - m);
  uint32_t i = 0, j = m, k = 0;
  while (i < m && j < n) tmp[k++] = (str_cmp(a[i], a[j]) <= 0) ? a[i++] : a[j++];
  while (i < m) tmp[k++] = a[i++];
  while (j < n) tmp[k++] = a[j++];
  memcpy(a, tmp, n * sizeof(Str));
}

// ---------------- Case Builder ----------------
static void run_case(SiteInput *in, Buf *out) {
  init_md_tables();
  StrMap tpls; map_init(&tpls, in->n_tpls + 8);
  Str *report = (Str *)arena_alloc(256 * sizeof(Str));
  uint32_t n_rep = 0, cap_rep = 256;
  #define PUSH_REP(msg) do { \
    if (n_rep == cap_rep) { \
      cap_rep *= 2; Str *nr = (Str *)arena_alloc(cap_rep * sizeof(Str)); \
      memcpy(nr, report, n_rep * sizeof(Str)); report = nr; \
    } report[n_rep++] = (msg); \
  } while (0)

  bool uses_esc_md = false;
  if (in->n_tpls > 1) {
    InputTpl *tmp = (InputTpl *)arena_alloc(in->n_tpls * sizeof(InputTpl));
    sort_tpls(in->tpls, tmp, in->n_tpls);
  }
  for (uint32_t i = 0; i < in->n_tpls; i++) {
    NodeList *nl = (NodeList *)arena_calloc(sizeof(NodeList));
    Str err;
    if (parse_template(in->tpls[i].name, in->tpls[i].src, nl, &uses_esc_md, &err)) {
      map_set(&tpls, in->tpls[i].name, nl);
    } else {
      PUSH_REP(err);
    }
  }
  bool pre_esc_md = !uses_esc_md;

  Str base = in->base_url;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;

  if (in->n_files > 1) {
    InputFile *tmp = (InputFile *)arena_alloc(in->n_files * sizeof(InputFile));
    sort_files(in->files, tmp, in->n_files);
  }

  SourceDoc *sources = (SourceDoc *)arena_calloc((in->n_files + 1) * sizeof(SourceDoc));
  uint32_t n_src = 0;
  InputFile *statics = (InputFile *)arena_alloc((in->n_files + 1) * sizeof(InputFile));
  uint32_t n_stat = 0;
  StrMap static_paths; map_init(&static_paths, in->n_files + 8);

  for (uint32_t i = 0; i < in->n_files; i++) {
    InputFile f = in->files[i];
    if (str_ends(f.path, S(".md"))) {
      bool has_cr = false;
      for (uint32_t k = 0; k < f.content.n; k++) if (f.content.p[k] == '\r') { has_cr = true; break; }
      Str txt = f.content;
      if (has_cr) {
        Buf nb; buf_init(&nb, f.content.n);
        for (uint32_t k = 0; k < f.content.n; k++) {
          if (f.content.p[k] == '\r') {
            buf_putc(&nb, '\n');
            if (k + 1 < f.content.n && f.content.p[k + 1] == '\n') k++;
          } else buf_putc(&nb, f.content.p[k]);
        }
        txt = str_lit(nb.d, nb.n);
      }
      sources[n_src].path = f.path;
      sources[n_src].text = txt;
      n_src++;
    } else {
      statics[n_stat++] = f;
      map_set(&static_paths, f.path, (void *)1);
    }
  }

  SourceDoc **kept = (SourceDoc **)arena_alloc((n_src + 1) * sizeof(SourceDoc *));
  uint32_t n_kept = 0;
  for (uint32_t i = 0; i < n_src; i++) {
    SourceDoc *s = &sources[i];
    Str err;
    if (!parse_frontmatter(s->text, &s->fm, &s->body, &err)) {
      PUSH_REP(fmt_err2(S("refused "), s->path, fmt_err2(S(": "), err, S(""))));
      continue;
    }
    s->is_post = str_starts(s->path, S("posts/"));
    if (!s->is_post) {
      if (str_starts(s->path, S("tags/"))) {
        PUSH_REP(fmt_err2(S("refused "), s->path, S(": tags/ is the generator's own")));
        continue;
      }
      if (str_starts(s->path, S("page/"))) {
        PUSH_REP(fmt_err2(S("refused "), s->path, S(": page/ is the generator's own")));
        continue;
      }
    }
    if (s->is_post && !s->fm.has_date) {
      PUSH_REP(fmt_err2(S("refused "), s->path, S(": a post needs a date")));
      continue;
    }
    s->layout = s->fm.has_layout ? s->fm.layout : (s->is_post ? S("post") : S("page"));
    if (!map_has(&tpls, s->layout)) {
      PUSH_REP(fmt_err2(S("refused "), s->path, fmt_err2(S(": layout \""), s->layout, S("\" is no template that parses"))));
      continue;
    }
    if (s->fm.has_draft && s->fm.draft) {
      PUSH_REP(fmt_err2(S("draft "), s->path, S("")));
      continue;
    }
    kept[n_kept++] = s;
  }

  StrMap slug_cache; map_init(&slug_cache, n_kept * 2 + 64);
  StrMap taken_dirs; map_init(&taken_dirs, 32);
  StrMap by_source; map_init(&by_source, n_kept + 8);

  for (uint32_t i = 0; i < n_kept; i++) {
    SourceDoc *s = kept[i];
    Str dir = s->is_post ? S("posts") : dir_of(s->path);
    StrMap *set = (StrMap *)map_get(&taken_dirs, dir);
    if (!set) {
      set = (StrMap *)arena_calloc(sizeof(StrMap));
      map_init(set, 32);
      if (dir.n == 0) {
        map_set(set, S("posts"), (void *)1);
        map_set(set, S("tags"), (void *)1);
        map_set(set, S("page"), (void *)1);
      }
      map_set(&taken_dirs, dir, set);
    }
    Str raw_slug = s->fm.has_slug ? s->fm.slug : slugify_cached(s->fm.title, &slug_cache);
    s->slug = unique_slug(raw_slug, set);
    if (dir.n == 0) s->out_dir = s->slug;
    else s->out_dir = fmt_err2(dir, S("/"), s->slug);
    s->url = fmt_err2(base, S("/"), fmt_err2(s->out_dir, S("/"), S("")));
    map_set(&by_source, s->path, s);
  }

  StrMap broken_map; map_init(&broken_map, 32);
  LinkCtx lc = {0};
  lc.base_url = base;
  lc.by_source = &by_source;
  lc.static_paths = &static_paths;
  lc.broken = &broken_map;

  for (uint32_t i = 0; i < n_kept; i++) {
    SourceDoc *s = kept[i];
    lc.cur_path = s->path;
    lc.cur_dir = dir_of(s->path);
    render_markdown(s, &lc, &slug_cache, pre_esc_md);
    uint32_t wc = words_of(s->body);
    s->reading_minutes = (wc == 0) ? 1 : ((wc + 199) / 200);
    s->date_long = s->fm.has_date ? long_date(s->fm.date) : str_lit("", 0);
  }

  SourceDoc **posts = (SourceDoc **)arena_alloc((n_kept + 1) * sizeof(SourceDoc *));
  uint32_t n_posts = 0;
  for (uint32_t i = 0; i < n_kept; i++) if (kept[i]->is_post) posts[n_posts++] = kept[i];
  if (n_posts > 1) {
    SourceDoc **tmp = (SourceDoc **)arena_alloc(n_posts * sizeof(SourceDoc *));
    sort_posts(posts, tmp, n_posts);
  }
  for (uint32_t k = 0; k < n_posts; k++) {
    posts[k]->newer = (k == 0) ? NULL : posts[k - 1];
    posts[k]->older = (k == n_posts - 1) ? NULL : posts[k + 1];
  }

  StrMap by_slug; map_init(&by_slug, 64);
  TagObj **tags_list = (TagObj **)arena_alloc(64 * sizeof(TagObj *));
  uint32_t n_tags_list = 0, cap_tags_list = 64;

  for (uint32_t i = 0; i < n_posts; i++) {
    SourceDoc *p = posts[i];
    if (p->fm.n_tags > 0) {
      StrMap seen; map_init(&seen, p->fm.n_tags + 4);
      TagObj **refs = (TagObj **)arena_alloc(p->fm.n_tags * sizeof(TagObj *));
      uint32_t nr = 0;
      for (uint32_t j = 0; j < p->fm.n_tags; j++) {
        Str name = p->fm.tags[j];
        Str sl = slugify_cached(name, &slug_cache);
        if (map_has(&seen, sl)) continue;
        map_set(&seen, sl, (void *)1);
        TagObj *t = (TagObj *)map_get(&by_slug, sl);
        if (!t) {
          t = (TagObj *)arena_calloc(sizeof(TagObj));
          t->name = name; t->slug = sl;
          t->url = fmt_err2(base, S("/tags/"), fmt_err2(sl, S("/"), S("")));
          t->cap_posts = 8;
          t->posts = (SourceDoc **)arena_alloc(8 * sizeof(SourceDoc *));
          map_set(&by_slug, sl, t);
          if (n_tags_list == cap_tags_list) {
            cap_tags_list *= 2;
            TagObj **nt = (TagObj **)arena_alloc(cap_tags_list * sizeof(TagObj *));
            memcpy(nt, tags_list, n_tags_list * sizeof(TagObj *));
            tags_list = nt;
          }
          tags_list[n_tags_list++] = t;
        }
        if (t->n_posts == t->cap_posts) {
          t->cap_posts *= 2;
          SourceDoc **np = (SourceDoc **)arena_alloc(t->cap_posts * sizeof(SourceDoc *));
          memcpy(np, t->posts, t->n_posts * sizeof(SourceDoc *));
          t->posts = np;
        }
        t->posts[t->n_posts++] = p;
        refs[nr++] = t;
      }
      p->tag_refs = refs; p->n_tag_refs = nr;
    }
  }

  for (uint32_t i = 0; i < n_kept; i++) {
    SourceDoc *s = kept[i];
    if (s->is_post) continue;
    if (s->fm.n_tags > 0) {
      StrMap seen; map_init(&seen, s->fm.n_tags + 4);
      TagObj **refs = (TagObj **)arena_alloc(s->fm.n_tags * sizeof(TagObj *));
      uint32_t nr = 0;
      for (uint32_t j = 0; j < s->fm.n_tags; j++) {
        Str name = s->fm.tags[j];
        Str sl = slugify_cached(name, &slug_cache);
        if (map_has(&seen, sl)) continue;
        map_set(&seen, sl, (void *)1);
        TagObj *t = (TagObj *)map_get(&by_slug, sl);
        if (!t) {
          t = (TagObj *)arena_calloc(sizeof(TagObj));
          t->name = name; t->slug = sl; t->url = str_lit("", 0);
        }
        refs[nr++] = t;
      }
      s->tag_refs = refs; s->n_tag_refs = nr;
    }
  }

  if (n_tags_list > 1) {
    TagObj **tmp = (TagObj **)arena_alloc(n_tags_list * sizeof(TagObj *));
    sort_tags(tags_list, tmp, n_tags_list);
  }

  CardObj *all_cards = (CardObj *)arena_calloc((n_kept + 1) * sizeof(CardObj));
  StrMap card_by_path; map_init(&card_by_path, n_kept + 8);
  for (uint32_t i = 0; i < n_kept; i++) {
    SourceDoc *s = kept[i];
    all_cards[i].doc = s;
    Val *ta = (Val *)arena_alloc((s->n_tag_refs + 1) * sizeof(Val));
    for (uint32_t j = 0; j < s->n_tag_refs; j++) {
      TagValObj *tv = (TagValObj *)arena_calloc(sizeof(TagValObj));
      tv->tag = s->tag_refs[j];
      ta[j] = (Val){ .t = VT_TAG, .ptr = tv };
    }
    all_cards[i].tags_arr = v_arr(ta, s->n_tag_refs);
    map_set(&card_by_path, s->path, &all_cards[i]);
  }

  Val *post_card_vals = (Val *)arena_alloc((n_posts + 1) * sizeof(Val));
  for (uint32_t i = 0; i < n_posts; i++) {
    CardObj *c = (CardObj *)map_get(&card_by_path, posts[i]->path);
    post_card_vals[i] = (Val){ .t = VT_CARD, .ptr = c };
  }

  Val *all_tags_vals = (Val *)arena_alloc((n_tags_list + 1) * sizeof(Val));
  for (uint32_t i = 0; i < n_tags_list; i++) {
    TagValObj *tv = (TagValObj *)arena_calloc(sizeof(TagValObj));
    tv->tag = tags_list[i];
    all_tags_vals[i] = (Val){ .t = VT_TAG, .ptr = tv };
  }

  SiteObj site_obj = { in->title, base };
  Val v_site = { .t = VT_SITE, .ptr = &site_obj };
  Val v_tags = v_arr(all_tags_vals, n_tags_list);
  uint32_t n_recent = (uint32_t)in->recent < n_posts ? (uint32_t)in->recent : n_posts;
  Val v_recent = v_arr(post_card_vals, n_recent);

  EvalCtx const_ec;
  const_ec.tpls = &tpls;
  const_ec.pre_esc_md = pre_esc_md;
  const_ec.n = 3;
  const_ec.binds[0] = (Binding){ S("site"), v_site };
  const_ec.binds[1] = (Binding){ S("tags"), v_tags };
  const_ec.binds[2] = (Binding){ S("recent"), v_recent };

  EvalCtx ec = const_ec;
  StrMap folded_tpls; map_init(&folded_tpls, in->n_tpls + 8);

  StrMap written; map_init(&written, n_kept + n_tags_list + n_stat + 32);
  bool first_key = true;

  buf_putc(out, '{');

  #define BEGIN_FILE(path_str) do { \
    map_set(&written, (path_str), (void *)1); \
    if (!first_key) buf_putc(out, ','); \
    first_key = false; \
    buf_putc(out, '"'); \
    buf_put_json(out, (path_str).p, (path_str).n); \
    buf_put(out, "\":\"", 3); \
  } while (0)

  // 1. Pages & Posts
  for (uint32_t i = 0; i < n_kept; i++) {
    SourceDoc *s = kept[i];
    NodeList *nl = get_folded_tpl(s->layout, &tpls, &folded_tpls, &const_ec);
    Val *toca = (Val *)arena_alloc((s->n_toc + 1) * sizeof(Val));
    for (uint32_t j = 0; j < s->n_toc; j++) toca[j] = (Val){ .t = VT_TOC, .ptr = &s->toc[j] };
    PageObj po = { s, &all_cards[i], v_arr(toca, s->n_toc) };
    ec.n = 4;
    ec.binds[3] = (Binding){ S("page"), (Val){ .t = VT_PAGE, .ptr = &po } };
    Str opath = fmt_err2(s->out_dir, S("/index.html"), S(""));
    BEGIN_FILE(opath);
    eval_nodes(out, &ec, nl, 0);
    buf_putc(out, '"');
  }

  // 2. Index pages
  NodeList *idx_nl = get_folded_tpl(S("index"), &tpls, &folded_tpls, &const_ec);
  if (idx_nl) {
    uint32_t per = (uint32_t)(in->per_page > 0 ? in->per_page : 1);
    uint32_t pages = n_posts == 0 ? 1 : (n_posts + per - 1) / per;
    for (uint32_t k = 1; k <= pages; k++) {
      uint32_t sidx = (k - 1) * per;
      uint32_t cnt = (sidx < n_posts) ? ((n_posts - sidx < per) ? n_posts - sidx : per) : 0;
      PagerObj pg;
      pg.number = (int32_t)k;
      pg.total = (int32_t)pages;
      Buf ub;
      if (k == 1) pg.url = fmt_err2(base, S("/"), S(""));
      else { buf_init(&ub, base.n + 16); buf_puts(&ub, base); buf_puts(&ub, S("/page/")); buf_put_u32(&ub, k); buf_putc(&ub, '/'); pg.url = str_lit(ub.d, ub.n); }
      if (k == 1) pg.newer_url = str_lit("", 0);
      else if (k == 2) pg.newer_url = fmt_err2(base, S("/"), S(""));
      else { buf_init(&ub, base.n + 16); buf_puts(&ub, base); buf_puts(&ub, S("/page/")); buf_put_u32(&ub, k - 1); buf_putc(&ub, '/'); pg.newer_url = str_lit(ub.d, ub.n); }
      if (k == pages) pg.older_url = str_lit("", 0);
      else { buf_init(&ub, base.n + 16); buf_puts(&ub, base); buf_puts(&ub, S("/page/")); buf_put_u32(&ub, k + 1); buf_putc(&ub, '/'); pg.older_url = str_lit(ub.d, ub.n); }

      ec.n = 5;
      ec.binds[3] = (Binding){ S("posts"), v_arr(post_card_vals + sidx, cnt) };
      ec.binds[4] = (Binding){ S("pager"), (Val){ .t = VT_PAGER, .ptr = &pg } };

      Str opath;
      if (k == 1) opath = S("index.html");
      else { buf_init(&ub, 24); buf_puts(&ub, S("page/")); buf_put_u32(&ub, k); buf_puts(&ub, S("/index.html")); opath = str_lit(ub.d, ub.n); }
      BEGIN_FILE(opath);
      eval_nodes(out, &ec, idx_nl, 0);
      buf_putc(out, '"');
    }
  }

  // 3. Tag pages
  NodeList *tag_nl = get_folded_tpl(S("tag"), &tpls, &folded_tpls, &const_ec);
  if (tag_nl) {
    for (uint32_t i = 0; i < n_tags_list; i++) {
      TagObj *t = tags_list[i];
      Val *tp = (Val *)arena_alloc((t->n_posts + 1) * sizeof(Val));
      for (uint32_t j = 0; j < t->n_posts; j++) {
        CardObj *c = (CardObj *)map_get(&card_by_path, t->posts[j]->path);
        tp[j] = (Val){ .t = VT_CARD, .ptr = c };
      }
      TagValObj tv = { t, true, v_arr(tp, t->n_posts) };
      ec.n = 4;
      ec.binds[3] = (Binding){ S("tag"), (Val){ .t = VT_TAG, .ptr = &tv } };
      Str opath = fmt_err2(S("tags/"), t->slug, S("/index.html"));
      BEGIN_FILE(opath);
      eval_nodes(out, &ec, tag_nl, 0);
      buf_putc(out, '"');
    }
  }

  // 4. Tags index
  NodeList *tags_nl = get_folded_tpl(S("tags"), &tpls, &folded_tpls, &const_ec);
  if (tags_nl) {
    ec.n = 3;
    BEGIN_FILE(S("tags/index.html"));
    eval_nodes(out, &ec, tags_nl, 0);
    buf_putc(out, '"');
  }

  // 5. Static files
  for (uint32_t i = 0; i < n_stat; i++) {
    if (map_has(&written, statics[i].path)) {
      PUSH_REP(fmt_err2(S("shadowed "), statics[i].path, S(": a built page has its path")));
    } else {
      BEGIN_FILE(statics[i].path);
      buf_put_json(out, statics[i].content.p, statics[i].content.n);
      buf_putc(out, '"');
    }
  }

  // 6. Report
  uint32_t total_lines = n_rep + lc.n_broken;
  Str *lines = (Str *)arena_alloc((total_lines + 1) * sizeof(Str));
  if (n_rep) memcpy(lines, report, n_rep * sizeof(Str));
  if (lc.n_broken) memcpy(lines + n_rep, lc.broken_list, lc.n_broken * sizeof(Str));
  if (total_lines > 1) {
    Str *tmp = (Str *)arena_alloc(total_lines * sizeof(Str));
    sort_strs(lines, tmp, total_lines);
  }
  BEGIN_FILE(S("build-report.txt"));
  for (uint32_t i = 0; i < total_lines; i++) {
    buf_put_json(out, lines[i].p, lines[i].n);
    buf_put(out, "\\n", 2);
  }
  Buf sum; buf_init(&sum, 128);
  buf_puts(&sum, S("built ")); buf_put_u32(&sum, n_kept - n_posts);
  buf_puts(&sum, S(" pages, ")); buf_put_u32(&sum, n_posts);
  buf_puts(&sum, S(" posts, ")); buf_put_u32(&sum, n_tags_list);
  buf_puts(&sum, S(" tags and ")); buf_put_u32(&sum, n_stat);
  buf_puts(&sum, S(" static files"));
  buf_put_json(out, sum.d, sum.n);
  buf_put(out, "\\n", 2);
  buf_putc(out, '"');

  buf_putc(out, '}');
}

// ---------------- Exports ----------------
static Buf g_out;

__attribute__((export_name("alloc_input")))
uint32_t alloc_input(uint32_t len) {
  arena_init();
  void *p = arena_alloc(len + 32);
  return (uint32_t)(uintptr_t)p;
}

__attribute__((export_name("get_out_ptr")))
uint32_t get_out_ptr(void) {
  return (uint32_t)(uintptr_t)g_out.d;
}

__attribute__((export_name("solve_all")))
uint32_t solve_all(uint32_t in_ptr, uint32_t in_len) {
  arena_init();
  buf_init(&g_out, 16 * 1024 * 1024);
  buf_putc(&g_out, '[');

  JP jp = { (const char *)(uintptr_t)in_ptr, in_len, 0 };
  jp_ws(&jp);
  if (jp.i < jp.n && jp.s[jp.i] == '[') jp.i++;
  jp_ws(&jp);

  bool first_case = true;
  while (jp.i < jp.n && jp.s[jp.i] != ']') {
    jp_ws(&jp);
    if (jp.s[jp.i] == '{') jp.i++;
    jp_ws(&jp);
    SiteInput si;
    bool has_in = false;
    while (jp.i < jp.n && jp.s[jp.i] != '}') {
      Str k = jp_str(&jp);
      jp_ws(&jp); jp.i++; // ':'
      if (str_eq(k, S("input"))) {
        si = parse_site_input(&jp);
        has_in = true;
      } else {
        jp_skip(&jp);
      }
      jp_ws(&jp);
      if (jp.i < jp.n && jp.s[jp.i] == ',') jp.i++;
      jp_ws(&jp);
    }
    if (jp.i < jp.n && jp.s[jp.i] == '}') jp.i++;
    if (has_in) {
      if (!first_case) buf_putc(&g_out, ',');
      first_case = false;
      run_case(&si, &g_out);
    }
    jp_ws(&jp);
    if (jp.i < jp.n && jp.s[jp.i] == ',') jp.i++;
    jp_ws(&jp);
  }

  buf_putc(&g_out, ']');
  return g_out.n;
}