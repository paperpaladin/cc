// cc -- a small C compiler for x86-64 Linux (System V ABI).
//
// Pipeline: tokenize -> parse -> emit AT&T assembly -> hand to gcc for as+ld.
//
// Deliberate simplifications, each with a known ceiling:
//   shortcut: stack-machine codegen, no register allocator. Output is ~3x the
//             instructions a real compiler emits. Fix when self-hosting is
//             green and `cc` is slow enough to care.
//   shortcut: nothing is ever freed. A compiler is a short-lived process; exit
//             reclaims. Add an arena only if we ever become a library.
//   shortcut: we exec gcc to assemble and link. Writing an assembler is a
//             separate project (see ../ld). Fix never, probably.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

// ---------------------------------------------------------------- diagnostics

// A translation unit is no longer one buffer. #include means any token can
// belong to any file that has been read, and the error reporter has to know
// which one.
typedef struct File File;
struct File {
  char *name;
  char *text;
  size_t len;
  File *next;
  int path_index;   // which include path found it, for #include_next
};
static File *files;   // every file read so far, newest first

static char *read_file(char *path);

// Translation phase 2: a backslash immediately before a newline splices the
// two lines into one. Done on the buffer rather than in the tokenizer
// because it is not a lexical rule -- it happens before anything is a token,
// which is why it can appear in the middle of a string literal or an
// identifier and still work.
//
// The deleted newlines are not simply dropped: the same number are put back
// at the end of the logical line, so every later line keeps the number it
// has in the file the user is looking at. Getting that wrong makes every
// diagnostic in a header full of continuations point at the wrong place.
static void splice_lines(char *p) {
  char *in = p, *out = p;
  int pending = 0;

  while (*in) {
    if (in[0] == '\\' && in[1] == '\n') {
      in += 2;
      pending++;
      continue;
    }
    if (*in == '\n') {
      *out++ = *in++;
      while (pending) { *out++ = '\n'; pending--; }
      continue;
    }
    *out++ = *in++;
  }
  while (pending) { *out++ = '\n'; pending--; }
  *out = 0;
}

static File *new_file(char *name, char *text) {
  File *f = calloc(1, sizeof(File));
  f->name = name;
  splice_lines(text);
  f->text = text;
  f->len  = strlen(text);
  f->path_index = -1;
  f->next = files;
  files = f;
  return f;
}

// Error locations arrive as a bare char* from 56 call sites, from before
// there was more than one file. Rather than thread a File through all of
// them, find the owner by range: the pointer came from some file's text, and
// the buffers do not overlap.
static File *file_of(char *loc) {
  for (File *f = files; f; f = f->next)
    if (f->text <= loc && loc <= f->text + f->len) return f;
  return NULL;
}

static void verror_at(char *loc, char *fmt, va_list ap) {
  File *src = file_of(loc);
  if (!src) {
    // A token the preprocessor synthesised (## paste, # stringify) points at
    // no file. Reporting a caret into someone else's buffer would be worse
    // than reporting no caret at all.
    fprintf(stderr, "cc: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
  }

  // Find the line containing loc.
  char *line = loc;
  while (src->text < line && line[-1] != '\n') line--;
  char *end = loc;
  while (*end && *end != '\n') end++;

  int lineno = 1;
  for (char *p = src->text; p < line; p++)
    if (*p == '\n') lineno++;

  int indent = fprintf(stderr, "%s:%d: ", src->name, lineno);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);
  fprintf(stderr, "%*s^ ", indent + (int)(loc - line), "");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

static void error_at(char *loc, char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  verror_at(loc, fmt, ap);
}

static void error(char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "cc: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

// ------------------------------------------------------------------ tokenizer

typedef enum { TK_IDENT, TK_PUNCT, TK_KEYWORD, TK_NUM, TK_STR, TK_EOF } TokenKind;

typedef struct Token Token;
typedef struct HideSet HideSet;

struct Token {
  TokenKind kind;
  Token *next;
  long val;      // TK_NUM
  char *loc;     // position inside file->text
  int len;
  char *str;     // TK_STR contents (NUL-terminated)
  int str_len;   // including the terminating NUL

  // Provenance. The preprocessor is line-oriented and the parser is not, so
  // these are set by the tokenizer and consumed before parsing begins.
  File *file;
  int line;
  bool num_long;      // TK_NUM: an L suffix, or too large for an int
  bool num_unsigned;  // TK_NUM: a U suffix
  bool at_bol;     // first token on its line: only here can a '#' be a directive
  bool has_space;  // whitespace preceded it, which '#' stringification must preserve
  HideSet *hide;   // macros this token may not be re-expanded by
};

// Tokenizer position, consumed by new_token. These are tokenizer state rather
// than parameters because new_token has 20-odd call sites in tokenize().
static File *tk_file;
static int  tk_line;
static bool tk_bol;
static bool tk_space;

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *t = calloc(1, sizeof(Token));
  t->kind = kind;
  t->loc = start;
  t->len = end - start;
  t->file = tk_file;
  t->line = tk_line;
  t->at_bol = tk_bol;
  t->has_space = tk_space;
  tk_bol = false;
  tk_space = false;
  return t;
}


static bool tok_is(Token *t, char *s) {
  return t->len == (int)strlen(s) && !memcmp(t->loc, s, t->len);
}

static bool starts_with(char *p, char *q) { return !strncmp(p, q, strlen(q)); }

static bool is_ident1(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident2(char c) { return is_ident1(c) || isdigit((unsigned char)c); }

static char *KEYWORDS[] = {
  "return", "if", "else", "for", "while", "do", "break", "continue",
  "int", "char", "long", "short", "void", "sizeof", "typedef",
  "struct", "union", "enum", "switch", "case", "default", "goto",
  "unsigned", "signed", "_Bool", "static", "extern", NULL,
};

static bool is_keyword(Token *t) {
  for (int i = 0; KEYWORDS[i]; i++)
    if (tok_is(t, KEYWORDS[i])) return true;
  return false;
}

// Longest-match punctuators. Order matters: 3-char before 2 before 1.
static char *PUNCT[] = {
  "<<=", ">>=", "...",
  "==", "!=", "<=", ">=", "&&", "||", "++", "--",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "->", "##",
  "+", "-", "*", "/", "%", "(", ")", "{", "}", "[", "]",
  "<", ">", "=", ";", ",", "&", "|", "^", "~", "!", "?", ":", ".", "#",
  NULL,
};

static int read_punct(char *p) {
  for (int i = 0; PUNCT[i]; i++)
    if (starts_with(p, PUNCT[i])) return strlen(PUNCT[i]);
  return 0;
}

static int from_hex(char c) {
  if ('0' <= c && c <= '9') return c - '0';
  if ('a' <= c && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

// Reads one (possibly escaped) character, advancing *pp past it.
static int read_char(char **pp) {
  char *p = *pp;
  if (*p != '\\') { *pp = p + 1; return *p; }
  p++;
  if ('0' <= *p && *p <= '7') {           // octal
    int v = 0;
    for (int i = 0; i < 3 && '0' <= *p && *p <= '7'; i++) v = v * 8 + (*p++ - '0');
    *pp = p; return v;
  }
  if (*p == 'x') {                         // hex
    p++;
    int v = 0;
    while (isxdigit((unsigned char)*p)) v = v * 16 + from_hex(*p++);
    *pp = p; return v;
  }
  int c = *p++;
  *pp = p;
  switch (c) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  case 'e': return 27;
  default:  return c;   // \\ \' \" and anything else is itself
  }
}

static Token *tokenize(File *file) {
  char *p = file->text;
  Token head = {0};
  Token *cur = &head;

  tk_file = file;
  tk_line = 1;
  tk_bol = true;
  tk_space = false;

  while (*p) {
    // A newline is not just whitespace here. It ends a directive and it is
    // what makes the next '#' a directive rather than an operator.
    if (*p == '\n') { p++; tk_line++; tk_bol = true; tk_space = false; continue; }
    if (isspace((unsigned char)*p)) { p++; tk_space = true; continue; }

    if (starts_with(p, "//")) {
      while (*p && *p != '\n') p++;
      tk_space = true;
      continue;
    }
    if (starts_with(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q) error_at(p, "unterminated block comment");
      for (char *s = p; s < q; s++) if (*s == '\n') tk_line++;
      p = q + 2;
      tk_space = true;  // a comment is whitespace, and may not join two tokens
      continue;
    }

    if (isdigit((unsigned char)*p)) {
      char *start = p;
      // strtoul, not strtol: a literal is never negative here (unary minus is
      // a separate node), and the range that matters is the full 64 bits.
      // strtol saturates at LONG_MAX, so 18446744073709551615 silently
      // became 9223372036854775807 rather than the bit pattern it names.
      long v = (long)strtoul(p, &p, 0);
      // The suffix is not noise. "1L << 32" is a 64-bit shift and "1 << 32"
      // is not, so a literal that says it is long has to be believed, and a
      // literal too large for an int is long whether it says so or not.
      bool suf_u = false, suf_l = false;
      while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') {
        if (*p == 'u' || *p == 'U') suf_u = true; else suf_l = true;
        p++;
      }
      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = v;
      cur->num_unsigned = suf_u;
      cur->num_long = suf_l || v > 2147483647L || v < -2147483648L;
      continue;
    }

    if (*p == '"') {
      char *start = p++;
      char buf[4096];
      int n = 0;
      while (*p != '"') {
        if (!*p || *p == '\n') error_at(start, "unterminated string literal");
        if (n >= (int)sizeof(buf) - 1) error_at(start, "string literal too long");
        buf[n++] = read_char(&p);
      }
      p++;
      cur = cur->next = new_token(TK_STR, start, p);
      cur->str = malloc(n + 1);
      memcpy(cur->str, buf, n);
      cur->str[n] = 0;
      cur->str_len = n + 1;
      continue;
    }

    if (*p == '\'') {
      char *start = p++;
      if (!*p) error_at(start, "unterminated char literal");
      int c = read_char(&p);
      if (*p != '\'') error_at(start, "unterminated char literal");
      p++;
      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = (char)c;   // char literals are signed on x86-64 Linux
      continue;
    }

    if (is_ident1(*p)) {
      char *start = p;
      while (is_ident2(*p)) p++;
      cur = cur->next = new_token(TK_IDENT, start, p);
      continue;
    }

    int len = read_punct(p);
    if (len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + len);
      p += len;
      continue;
    }

    error_at(p, "invalid character '%c'", *p);
  }

  tk_bol = true;
  cur->next = new_token(TK_EOF, p, p);
  return head.next;
}

// Deferred out of tokenize(). Until the preprocessor has run, "int" might be
// a macro parameter and a macro might expand to "return"; deciding keyword-ness
// at tokenize time would freeze the wrong answer in both directions.
static void convert_keywords(Token *t) {
  for (; t && t->kind != TK_EOF; t = t->next)
    if (t->kind == TK_IDENT && is_keyword(t)) t->kind = TK_KEYWORD;
}

// ----------------------------------------------------------------- type system

typedef enum {
  TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_ENUM,
  TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNC,
  // Declarable, not operable. A real header declares floating point long
  // before anything calls it -- glibc reaches "typedef float _Float32" on
  // the way to printf -- so refusing the TYPE blocks headers that have
  // nothing to do with arithmetic. These parse, size correctly and take
  // part in declarations; producing a value of one is a named error.
  TY_FLOAT, TY_DOUBLE,
} TypeKind;

typedef struct Member Member;
typedef struct Type Type;

struct Type {
  TypeKind kind;
  int size;
  int align;         // a struct's alignment is its widest member's
  bool is_unsigned;
  Type *base;        // TY_PTR / TY_ARRAY element type
  int array_len;
  Type *ret;         // TY_FUNC
  Member *members;   // TY_STRUCT / TY_UNION
};

struct Member {
  Member *next;
  char *name;
  Type *ty;
  int offset;
  // Bitfields. offset still names the storage unit this member is read
  // through; bit_offset is where inside it the value starts, counting from
  // the least significant bit, and bit_width is how many bits it is.
  bool is_bitfield;
  int bit_offset;
  int bit_width;
};

static int align_to(int n, int a);

// Designated initialisers, so adding a field to Type does not produce a
// missing-initialiser warning on every one of these.
#define PRIM(k, sz, uns) &(Type){.kind = (k), .size = (sz), .align = (sz), .is_unsigned = (uns)}
static Type *ty_void   = PRIM(TY_VOID,  1, false);
static Type *ty_bool   = PRIM(TY_BOOL,  1, false);
static Type *ty_char   = PRIM(TY_CHAR,  1, false);
static Type *ty_uchar  = PRIM(TY_CHAR,  1, true);
static Type *ty_short  = PRIM(TY_SHORT, 2, false);
static Type *ty_ushort = PRIM(TY_SHORT, 2, true);
static Type *ty_int    = PRIM(TY_INT,   4, false);
static Type *ty_uint   = PRIM(TY_INT,   4, true);
static Type *ty_long   = PRIM(TY_LONG,  8, false);
static Type *ty_ulong  = PRIM(TY_LONG,  8, true);
static Type *ty_float  = PRIM(TY_FLOAT,  4, false);
static Type *ty_double = PRIM(TY_DOUBLE, 8, false);
// x87 long double is 80 bits of value in 16 bytes of storage. Nothing here
// can do arithmetic on one, but sizeof and struct layout are not arithmetic,
// and a wrong size is a wrong answer rather than a missing feature. PLAN.md
// used to propose aliasing this to double and documenting the lie; the
// differential suite objected the moment the type became declarable, which
// is a better argument than the one in the plan.
static Type *ty_ldouble = PRIM(TY_DOUBLE, 16, false);
#undef PRIM

static Type *pointer_to(Type *base) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_PTR; t->size = 8; t->align = 8; t->base = base;
  t->is_unsigned = true;            // addresses compare as unsigned
  return t;
}

static Type *array_of(Type *base, int len) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_ARRAY; t->size = base->size * len; t->align = base->align;
  t->base = base; t->array_len = len;
  return t;
}

static Type *func_returning(Type *ret) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_FUNC; t->size = 8; t->align = 8; t->ret = ret;
  return t;
}

static Type *new_enum_type(void) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_ENUM; t->size = 4; t->align = 4;
  return t;
}

static bool is_integer(Type *t) {
  switch (t->kind) {
  case TY_BOOL: case TY_CHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_ENUM:
    return true;
  default:
    return false;
  }
}
static bool is_pointer(Type *t) { return t->kind == TY_PTR || t->kind == TY_ARRAY; }
static bool is_aggregate(Type *t) { return t->kind == TY_STRUCT || t->kind == TY_UNION; }

// An aggregate and an array both evaluate to their own address rather than to a
// loadable value, which is the single rule that makes member access, array
// indexing and struct copying all fall out of the same codegen.
// A function joins arrays and aggregates here: its "value" is its address.
// Without it, loading a function designator emits mov (%rax), %rax and
// dereferences the first instruction of the code.
static bool is_address_valued(Type *t) {
  return t->kind == TY_ARRAY || t->kind == TY_FUNC || is_aggregate(t);
}

// ------------------------------------------------------------------------ AST

typedef enum {
  ND_NUM, ND_VAR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_NEG,
  ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_ADDR, ND_DEREF,
  ND_AND, ND_OR, ND_NOT, ND_BITAND, ND_BITOR, ND_BITXOR, ND_BITNOT,
  ND_SHL, ND_SHR, ND_COND, ND_COMMA, ND_CAST, ND_MEMBER,
  ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_BLOCK, ND_FUNCALL, ND_EXPR_STMT,
  ND_VA_START, ND_VA_ARG,
  ND_BREAK, ND_CONTINUE, ND_SWITCH, ND_CASE, ND_GOTO, ND_LABEL,
} NodeKind;

typedef struct Node Node;
typedef struct Obj Obj;
typedef struct Relocation Relocation;

// A slot in a global's initialiser whose value is an address, and so cannot
// be a byte pattern: the linker has to fill it in. "char *p = \"abc\";" is
// eight bytes of nothing plus the instruction to put the string's address
// there. Kept in a sorted list so emission can walk bytes and relocations
// together in one pass.
struct Relocation {
  Relocation *next;
  int offset;
  char *label;
  long addend;
};

struct Obj {
  Obj *next;
  char *name;
  Type *ty;
  bool is_local;
  bool is_function;
  bool is_definition;
  bool is_anon_str;    // a string literal's storage: .rodata, never .globl
  bool is_static;      // internal linkage: no .globl
  bool is_extern;      // declared elsewhere: reference it, do not define it
  // local
  int offset;
  // global data
  char *init_data;   // the bytes, when there is an initialiser
  int init_len;
  Relocation *rel;   // the places in those bytes that hold an address
  // function
  Obj *params;
  Obj *locals;
  Node *body;
  int stack_size;
  bool is_variadic;   // declared with a trailing "..."
  int va_area;        // rbp-relative base of the register save area
  bool has_proto;     // "f(void)" or "f(int)" promise an argument list;
                      // "f()" in C promises nothing, so it is not checked
};

struct Node {
  NodeKind kind;
  Node *next;      // statement list
  Type *ty;
  Token *tok;      // for diagnostics

  Node *lhs, *rhs;

  // if / for / do
  Node *cond, *then, *els, *init, *inc;

  Node *body;      // ND_BLOCK

  // ND_FUNCALL
  char *funcname;  // direct call: the symbol to call
  Node *args;
  Obj *fn;         // the declaration, when there is one
  Node *callee;    // indirect call: the expression yielding the address
  Obj *ret_buf;    // a call returning a small aggregate needs somewhere to
                   // put it: the value arrives in rax:rdx, but every
                   // aggregate in this compiler is addressed, not held.

  Member *member;  // ND_MEMBER

  // ND_SWITCH / ND_CASE
  Node *case_next;       // next case within the enclosing switch
  Node *default_case;
  long case_val;
  char *brk_lbl;         // labels assigned during codegen
  char *cont_lbl;
  char *case_lbl;

  // ND_GOTO / ND_LABEL
  char *label;
  char *unique_label;
  Node *goto_next;

  Obj *var;        // ND_VAR
  long val;        // ND_NUM
};

static Node *new_node(NodeKind kind, Token *tok) {
  Node *n = calloc(1, sizeof(Node));
  n->kind = kind; n->tok = tok;
  return n;
}
static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *n = new_node(kind, tok); n->lhs = lhs; n->rhs = rhs; return n;
}
static Node *new_unary(NodeKind kind, Node *lhs, Token *tok) {
  Node *n = new_node(kind, tok); n->lhs = lhs; return n;
}
static Node *new_num(long v, Token *tok) {
  Node *n = new_node(ND_NUM, tok); n->val = v; n->ty = ty_int; return n;
}
static void add_type(Node *n);

static Node *new_cast(Node *expr, Type *ty) {
  // The operand must be typed before it is wrapped, and the caller cannot be
  // relied on to have done it: cast_expr() builds "(long)f()" out of a call
  // node nobody has typed yet, and codegen then asks a NULL type what kind
  // it is. Every other caller happened to pass something already typed,
  // which is why casting a function call crashed the compiler and nothing
  // noticed until it was pointed at its own source.
  add_type(expr);
  Node *n = calloc(1, sizeof(Node));
  n->kind = ND_CAST;
  n->tok = expr->tok;
  n->lhs = expr;
  n->ty = ty;
  return n;
}

static Node *new_var_node(Obj *var, Token *tok) {
  Node *n = new_node(ND_VAR, tok); n->var = var; n->ty = var->ty; return n;
}

// Propagate types bottom-up. Called once per node after it is built.
// What may appear on the left of an assignment. Deliberately a whitelist:
// anything not named here is not assignable, which is the safe direction to
// be wrong in.
static bool is_lvalue(Node *n) {
  switch (n->kind) {
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER:
    return true;
  case ND_COMMA:
    return is_lvalue(n->rhs);
  default:
    return false;
  }
}

static void add_type(Node *n) {
  if (!n || n->ty) return;

  add_type(n->lhs); add_type(n->rhs);
  add_type(n->cond); add_type(n->then); add_type(n->els);
  add_type(n->init); add_type(n->inc);
  for (Node *b = n->body; b; b = b->next) add_type(b);
  for (Node *a = n->args; a; a = a->next) add_type(a);

  switch (n->kind) {
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR:
    // Integer promotion, crudely: anything narrower than long becomes int,
    // and if either side is long the result is long.
    n->ty = (n->lhs->ty->kind == TY_LONG || n->rhs->ty->kind == TY_LONG) ? ty_long : ty_int;
    return;
  case ND_NEG: case ND_BITNOT: case ND_SHL: case ND_SHR:
    n->ty = (n->lhs->ty->kind == TY_LONG) ? ty_long : ty_int;
    return;
  case ND_ASSIGN:
    if (n->lhs->ty->kind == TY_ARRAY) error_at(n->tok->loc, "not an lvalue");
    // Assignability is a question about the expression, not about whether
    // codegen can produce an address for it. gen_addr() will happily give
    // the address of a struct-returning call, because that is how such a
    // call is passed on to something else -- but "f() = x" is still not C.
    if (!is_lvalue(n->lhs))
      error_at(n->tok->loc, "lvalue required as the left operand of assignment");
    if (is_aggregate(n->lhs->ty) && n->lhs->ty != n->rhs->ty &&
        n->lhs->ty->size != n->rhs->ty->size)
      error_at(n->tok->loc, "incompatible types in struct assignment");
    n->ty = n->lhs->ty;
    return;
  case ND_MEMBER:
    n->ty = n->member->ty;
    return;
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
  case ND_AND: case ND_OR: case ND_NOT:
    n->ty = ty_int;
    return;
  case ND_VAR:
    n->ty = n->var->ty;
    return;
  case ND_COND:
    n->ty = n->then->ty;
    return;
  case ND_COMMA:
    n->ty = n->rhs->ty;
    return;
  case ND_ADDR:
    // &arr has type "pointer to element", matching C's decay rules closely
    // enough for everything we support.
    n->ty = (n->lhs->ty->kind == TY_ARRAY) ? pointer_to(n->lhs->ty->base)
                                           : pointer_to(n->lhs->ty);
    return;
  case ND_DEREF:
    if (!is_pointer(n->lhs->ty)) error_at(n->tok->loc, "invalid pointer dereference");
    if (n->lhs->ty->base->kind == TY_VOID) error_at(n->tok->loc, "dereferencing void pointer");
    n->ty = n->lhs->ty->base;
    return;
  case ND_FUNCALL:
    // A declared function has a return type. An undeclared one does not, and
    // C89's implicit "int" would silently truncate every pointer returned by
    // an undeclared malloc, so the older and wider assumption is kept for
    // that case and the README says so.
    // shortcut: implicit declarations assume long. Make them an error once
    // cc.c itself declares everything it calls.
    if (n->callee) return;          // set when the call node was built
    n->ty = n->fn ? n->fn->ty->ret : ty_long;
    return;
  default:
    return;
  }
}

// ----------------------------------------------------------------- the parser

static Token *tk;              // cursor
static Obj *locals;            // every local in the current function, for stack
                               // allocation. Lookups go through `scope`, not here.
static Obj *globals;
static char *brk_label;        // innermost loop labels, or NULL
static char *cont_label;
static int label_counter = 1;  // names user labels at parse time
static Node *current_switch;   // innermost switch, so case/default can attach
static Node *gotos;            // every goto in the current function
static Node *labels;           // every label in the current function

// One entry per name visible in a scope. A C identifier can be a variable, a
// typedef, or an enum constant, and which one it is depends on the innermost
// scope that declares it -- so all three live in the same list and shadow each
// other correctly for free.
typedef struct VarScope VarScope;
struct VarScope {
  VarScope *next;
  char *name;
  Obj *var;
  Type *type_def;
  Type *enum_ty;
  int enum_val;
};

// struct/union/enum tags live in a separate namespace from ordinary identifiers,
// which is why `struct stat stat;` is legal C.
typedef struct TagScope TagScope;
struct TagScope {
  TagScope *next;
  char *name;
  Type *ty;
};

typedef struct Scope Scope;
struct Scope {
  Scope *next;
  VarScope *vars;
  TagScope *tags;
};

static Scope file_scope;
static Scope *scope = &file_scope;

// Storage-class bits picked up by declspec and handed back to its caller.
typedef struct { bool is_typedef; bool is_static; bool is_extern; } VarAttr;

static void enter_scope(void) {
  Scope *s = calloc(1, sizeof(Scope));
  s->next = scope;
  scope = s;
}

static void leave_scope(void) { scope = scope->next; }

static VarScope *push_scope(char *name) {
  VarScope *vs = calloc(1, sizeof(VarScope));
  vs->name = name;
  vs->next = scope->vars;
  scope->vars = vs;
  return vs;
}

static void push_tag(char *name, Type *ty) {
  TagScope *ts = calloc(1, sizeof(TagScope));
  ts->name = name;
  ts->ty = ty;
  ts->next = scope->tags;
  scope->tags = ts;
}

static bool name_is(char *name, Token *t) {
  return (int)strlen(name) == t->len && !memcmp(t->loc, name, t->len);
}

static bool consume(char *s) {
  if (tok_is(tk, s)) { tk = tk->next; return true; }
  return false;
}
static void expect(char *s) {
  if (!tok_is(tk, s)) error_at(tk->loc, "expected '%s'", s);
  tk = tk->next;
}
static bool peek(char *s) { return tok_is(tk, s); }

static char *get_ident(void) {
  if (tk->kind != TK_IDENT) error_at(tk->loc, "expected an identifier");
  char *s = strndup(tk->loc, tk->len);
  tk = tk->next;
  return s;
}

static Obj *new_var(char *name, Type *ty, bool is_local) {
  Obj *v = calloc(1, sizeof(Obj));
  v->name = name; v->ty = ty; v->is_local = is_local;
  if (is_local) { v->next = locals; locals = v; }
  else          { v->next = globals; globals = v; }
  // An unnamed parameter -- "int f(int, int b)" -- still needs its Obj, so
  // that it occupies an argument register and the parameters after it land in
  // the right ones. What it must not get is a scope entry: nothing can refer
  // to it, and a NULL name in the scope list is a strlen() crash waiting for
  // the first lookup that has to scan past it.
  if (name) push_scope(name)->var = v;
  return v;
}

// Innermost scope wins, which is what makes shadowing work.
static VarScope *find_name(Token *t) {
  for (Scope *s = scope; s; s = s->next)
    for (VarScope *vs = s->vars; vs; vs = vs->next)
      if (name_is(vs->name, t)) return vs;
  return NULL;
}

static Type *find_tag(Token *t) {
  for (Scope *s = scope; s; s = s->next)
    for (TagScope *ts = s->tags; ts; ts = ts->next)
      if (name_is(ts->name, t)) return ts->ty;
  return NULL;
}

// A tag declared in *this* scope only -- needed to tell a redeclaration
// (struct foo { ... } again) from a reference to an outer one.
static TagScope *find_tag_here(Token *t) {
  for (TagScope *ts = scope->tags; ts; ts = ts->next)
    if (name_is(ts->name, t)) return ts;
  return NULL;
}

static Type *find_typedef(Token *t) {
  if (t->kind != TK_IDENT) return NULL;
  VarScope *vs = find_name(t);
  return vs ? vs->type_def : NULL;
}

static int anon_id = 0;
static Obj *new_string_literal(Token *t) {
  char name[32];
  snprintf(name, sizeof(name), ".L.str.%d", anon_id++);
  Obj *v = new_var(strdup(name), array_of(ty_char, t->str_len), false);
  v->init_data = t->str;
  v->init_len = t->str_len;
  v->is_anon_str = true;
  return v;
}

static Node *expr(void);
static Node *assign(void);
static Node *conditional(void);
// Pointer arithmetic lives in new_add/new_sub, which the initialiser walker needs
// in order to build a[i] as *(a+i) and get the element scaling for free.
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Node *stmt(void);
static Node *compound_stmt(void);
static long eval(Node *n);
static bool in_function_body;
static int aggregate_regs(Type *ty);
static Type *declspec(VarAttr *attr);
static Type *declarator(Type *ty, char **name);
static Type *struct_union_decl(void);
static Type *enum_decl(void);
static long const_expr(void);

static char *TYPE_KEYWORDS[] = {
  "int", "char", "long", "short", "void", "unsigned", "signed", "_Bool",
  "struct", "union", "enum", "typedef", "static", "extern",
  "float", "double",
  "const", "volatile", "restrict", "__restrict", "__restrict__",
  "inline", "__inline", "__inline__", "_Noreturn",
  NULL,
};

// Qualifiers this compiler parses and then ignores. const and volatile are
// not enforced -- there is no const-checking here and saying so is better
// than pretending -- and restrict is an optimisation promise to a code
// generator that does not use it. Skipping them is what lets a real header
// declare "void *memcpy(void *__restrict, const void *__restrict, size_t)".
static bool skip_qualifiers(void) {
  static char *QUALS[] = {
    "const", "volatile", "restrict", "__restrict", "__restrict__",
    "inline", "__inline", "__inline__", "_Noreturn", NULL,
  };
  bool any = false;
  for (;;) {
    int i = 0;
    for (; QUALS[i]; i++)
      if (peek(QUALS[i])) { tk = tk->next; any = true; break; }
    if (!QUALS[i]) return any;
  }
}

// A declaration can start with a type keyword, a storage class, or a name that
// an enclosing scope made into a typedef. That last case is why this needs the
// scope stack and cannot be a pure lexical test.
static bool is_typename(void) {
  for (int i = 0; TYPE_KEYWORDS[i]; i++)
    if (peek(TYPE_KEYWORDS[i])) return true;
  return find_typedef(tk) != NULL;
}

// declspec = ("int" | "char" | "short" | "long" | "void")+
// We accept the common multi-word spellings ("long long", "unsigned" is not
// supported) and collapse them to one of our five base types.
static Type *declspec(VarAttr *attr) {
  // C lets the specifiers of a declaration appear in any order, so
  // "long unsigned int", "unsigned long" and "int unsigned long" all name
  // the same type. Positional parsing gets the common spellings and then
  // fails on a real header: glibc's stddef.h writes "long unsigned int".
  //
  // So each keyword adds to a counter instead, and the total names the type.
  // LONG adds 1<<10 so that a second "long" lands on its own bit rather than
  // colliding with anything, which is what makes "long long" fall out
  // without a special case.
  enum {
    M_VOID     = 1 << 0,
    M_BOOL     = 1 << 2,
    M_CHAR     = 1 << 4,
    M_SHORT    = 1 << 6,
    M_INT      = 1 << 8,
    M_LONG     = 1 << 10,
    M_FLOAT    = 1 << 16,
    M_DOUBLE   = 1 << 18,
    M_OTHER    = 1 << 12,
    M_SIGNED   = 1 << 13,
    M_UNSIGNED = 1 << 14,
  };

  Type *ty = ty_int;
  int counter = 0;

  while (is_typename()) {
    if (skip_qualifiers()) continue;

    // Storage classes may appear anywhere among the specifiers, including
    // after them: "int static x;" is legal, if unloved.
    if (peek("typedef")) {
      if (!attr) error_at(tk->loc, "typedef is not allowed here");
      attr->is_typedef = true;
      tk = tk->next;
      continue;
    }
    if (peek("static") || peek("extern")) {
      if (!attr) error_at(tk->loc, "a storage class is not allowed here");
      if (peek("static")) attr->is_static = true; else attr->is_extern = true;
      if (attr->is_static && attr->is_extern)
        error_at(tk->loc, "static and extern are mutually exclusive");
      tk = tk->next;
      continue;
    }

    // A tag or a typedef name is a complete type on its own, and cannot be
    // combined with the keywords above.
    if (peek("struct") || peek("union") || peek("enum") || find_typedef(tk)) {
      if (counter) break;
      if (peek("struct") || peek("union")) ty = struct_union_decl();
      else if (peek("enum")) ty = enum_decl();
      else { ty = find_typedef(tk); tk = tk->next; }
      counter += M_OTHER;
      continue;
    }

    if (consume("void"))          counter += M_VOID;
    else if (consume("_Bool"))    counter += M_BOOL;
    else if (consume("char"))     counter += M_CHAR;
    else if (consume("short"))    counter += M_SHORT;
    else if (consume("int"))      counter += M_INT;
    else if (consume("long"))     counter += M_LONG;
    else if (consume("float"))    counter += M_FLOAT;
    else if (consume("double"))   counter += M_DOUBLE;
    else if (consume("signed"))   counter |= M_SIGNED;
    else if (consume("unsigned")) counter |= M_UNSIGNED;
    else break;

    switch (counter) {
    case M_VOID:
      ty = ty_void; break;
    case M_BOOL:
      ty = ty_bool; break;
    case M_CHAR:
    case M_SIGNED + M_CHAR:
      ty = ty_char; break;
    case M_UNSIGNED + M_CHAR:
      ty = ty_uchar; break;
    case M_SHORT:
    case M_SHORT + M_INT:
    case M_SIGNED + M_SHORT:
    case M_SIGNED + M_SHORT + M_INT:
      ty = ty_short; break;
    case M_UNSIGNED + M_SHORT:
    case M_UNSIGNED + M_SHORT + M_INT:
      ty = ty_ushort; break;
    case M_INT:
    case M_SIGNED:
    case M_SIGNED + M_INT:
      ty = ty_int; break;
    case M_UNSIGNED:
    case M_UNSIGNED + M_INT:
      ty = ty_uint; break;
    case M_LONG:
    case M_LONG + M_INT:
    case M_LONG + M_LONG:
    case M_LONG + M_LONG + M_INT:
    case M_SIGNED + M_LONG:
    case M_SIGNED + M_LONG + M_INT:
    case M_SIGNED + M_LONG + M_LONG:
    case M_SIGNED + M_LONG + M_LONG + M_INT:
      ty = ty_long; break;
    case M_UNSIGNED + M_LONG:
    case M_UNSIGNED + M_LONG + M_INT:
    case M_UNSIGNED + M_LONG + M_LONG:
    case M_UNSIGNED + M_LONG + M_LONG + M_INT:
      ty = ty_ulong; break;
    case M_FLOAT:
      ty = ty_float; break;
    case M_DOUBLE:
      ty = ty_double; break;
    case M_LONG + M_DOUBLE:
      ty = ty_ldouble; break;
    default:
      error_at(tk->loc, "these type specifiers cannot be combined");
    }
  }

  return ty;
}

// Trailing array dimensions: "[3][4]" applied right-to-left.
// Fold an integer constant expression. Needed for enum values and array
// dimensions, both of which must be known at compile time.
// A constant expression whose value may be an address. *label comes back
// non-NULL for "the address of that symbol, plus this much", which is the
// only form a linker can resolve and therefore the only one a global
// initialiser may contain beyond a plain number.
//
// Arrays, functions and string literals are their own addresses; everything
// else has to be written with a & to get one.
static long eval_addr(Node *n, char **label) {
  switch (n->kind) {
  case ND_ADD:   return eval_addr(n->lhs, label) + eval_addr(n->rhs, label);
  case ND_SUB:   return eval_addr(n->lhs, label) - eval_addr(n->rhs, label);
  case ND_CAST:  return eval_addr(n->lhs, label);
  case ND_ADDR:
    if (n->lhs->kind != ND_VAR || n->lhs->var->is_local)
      error_at(n->tok->loc, "a global initialiser can only take the address of a global");
    *label = n->lhs->var->name;
    return 0;
  case ND_VAR:
    if (n->var->is_local)
      error_at(n->tok->loc, "a global initialiser cannot refer to a local");
    if (n->var->ty->kind != TY_ARRAY && !n->var->is_function)
      error_at(n->tok->loc, "a global initialiser cannot read another variable");
    *label = n->var->name;
    return 0;
  case ND_MEMBER:
    // &s.field, reached through the ND_ADDR case above having unwrapped.
    error_at(n->tok->loc, "a struct member is not a constant address here");
    return 0;
  default:
    return eval(n);
  }
}

static long eval(Node *n) {
  switch (n->kind) {
  case ND_NUM:    return n->val;
  case ND_ADD:    return eval(n->lhs) + eval(n->rhs);
  case ND_SUB:    return eval(n->lhs) - eval(n->rhs);
  case ND_MUL:    return eval(n->lhs) * eval(n->rhs);
  case ND_DIV:    { long d = eval(n->rhs); if (!d) error_at(n->tok->loc, "division by zero in a constant expression"); return eval(n->lhs) / d; }
  case ND_MOD:    { long d = eval(n->rhs); if (!d) error_at(n->tok->loc, "division by zero in a constant expression"); return eval(n->lhs) % d; }
  case ND_NEG:    return -eval(n->lhs);
  case ND_BITNOT: return ~eval(n->lhs);
  case ND_NOT:    return !eval(n->lhs);
  case ND_BITAND: return eval(n->lhs) & eval(n->rhs);
  case ND_BITOR:  return eval(n->lhs) | eval(n->rhs);
  case ND_BITXOR: return eval(n->lhs) ^ eval(n->rhs);
  case ND_SHL:    return eval(n->lhs) << eval(n->rhs);
  case ND_SHR:    return eval(n->lhs) >> eval(n->rhs);
  case ND_EQ:     return eval(n->lhs) == eval(n->rhs);
  case ND_NE:     return eval(n->lhs) != eval(n->rhs);
  case ND_LT:     return eval(n->lhs) <  eval(n->rhs);
  case ND_LE:     return eval(n->lhs) <= eval(n->rhs);
  case ND_AND:    return eval(n->lhs) && eval(n->rhs);
  case ND_OR:     return eval(n->lhs) || eval(n->rhs);
  case ND_COND:   return eval(n->cond) ? eval(n->then) : eval(n->els);
  case ND_COMMA:  return eval(n->rhs);
  case ND_CAST:   return eval(n->lhs);
  default:
    error_at(n->tok->loc, "not a compile-time constant");
    return 0;
  }
}

static long const_expr(void) { return eval(conditional()); }

static Type *type_suffix(Type *ty) {
  // A function type, as in "typedef int handler_t(void *, long);". Only the
  // return type is kept: nothing here calls through such a typedef, it is
  // used to declare pointers, and a pointer to a function is eight bytes
  // whatever the parameters are. Recording them would be work with no
  // reader. The declaration still has to be *consumed*, though, or the
  // parser resumes in the middle of a parameter list.
  if (consume("(")) {
    int depth = 0;
    while (!(depth == 0 && peek(")"))) {
      if (tk->kind == TK_EOF) error_at(tk->loc, "unterminated parameter list");
      if (tok_is(tk, "(")) depth++;
      else if (tok_is(tk, ")")) depth--;
      tk = tk->next;
    }
    expect(")");
    return func_returning(ty);
  }

  if (consume("[")) {
    // An omitted dimension means "infer it from the initialiser"; it is recorded
    // as -1 and patched in declaration() once the initialiser has been counted.
    if (consume("]")) return array_of(type_suffix(ty), -1);
    long len = const_expr();
    if (len < 0) error_at(tk->loc, "negative array size");
    expect("]");
    return array_of(type_suffix(ty), (int)len);
  }
  return ty;
}

// Move the cursor past a balanced "( ... )" that starts at the cursor.
static void skip_parens(void) {
  int depth = 0;
  do {
    if (tk->kind == TK_EOF) error_at(tk->loc, "unbalanced parentheses in a declarator");
    if (tok_is(tk, "(")) depth++;
    else if (tok_is(tk, ")")) depth--;
    tk = tk->next;
  } while (depth);
}

// declarator = "*"* ( ident | "(" declarator ")" ) type-suffix*
//
// The parenthesised form is what makes "int (*fp)(int)" a pointer to a
// function rather than a function returning a pointer, and it cannot be
// parsed in one pass: the suffix AFTER the closing paren -- here "(int)" --
// describes what the thing inside the parens points AT, so it has to be read
// before the inside means anything.
//
// So the parens are skipped, the outer suffix is parsed to build the type
// the inner declarator should be handed, and then the cursor goes back and
// reads the inside against it. Two passes over a handful of tokens, and the
// alternative is threading a half-built type through the recursion.
//
// *name comes back NULL for an abstract declarator, which is legal in a
// parameter list: "void (*)(void)" names nothing.
static Type *declarator(Type *ty, char **name) {
  *name = NULL;
  skip_qualifiers();
  while (consume("*")) { ty = pointer_to(ty); skip_qualifiers(); }

  if (peek("(")) {
    Token *inner = tk->next;
    skip_parens();                 // cursor now sits on the outer suffix
    ty = type_suffix(ty);          // which is what the inside points at
    Token *after = tk;

    tk = inner;
    Type *res = declarator(ty, name);
    if (!peek(")")) error_at(tk->loc, "expected ')' to close a declarator");
    tk = after;
    return res;
  }

  if (tk->kind == TK_IDENT) *name = get_ident();
  return type_suffix(ty);
}

// struct-or-union-specifier = ("struct" | "union") ident? ("{" members "}")?
static Type *struct_union_decl(void) {
  bool is_union = peek("union");
  tk = tk->next;

  Token *tag = NULL;
  if (tk->kind == TK_IDENT) { tag = tk; tk = tk->next; }

  // A tag with no body is a reference, or a forward declaration if the tag is
  // not known yet. Incomplete types are marked with size -1 so that using one
  // by value is caught rather than silently producing a zero-sized object.
  if (tag && !peek("{")) {
    Type *known = find_tag(tag);
    if (known) return known;
    Type *ty = calloc(1, sizeof(Type));
    ty->kind = is_union ? TY_UNION : TY_STRUCT;
    ty->size = -1;
    ty->align = 1;
    push_tag(strndup(tag->loc, tag->len), ty);
    return ty;
  }

  expect("{");
  Type *ty = calloc(1, sizeof(Type));
  ty->kind = is_union ? TY_UNION : TY_STRUCT;
  ty->align = 1;

  Member head = {0};
  Member *cur = &head;
  while (!consume("}")) {
    Type *base = declspec(NULL);
    bool first = true;
    while (!consume(";")) {
      if (!first) expect(",");
      first = false;
      // An unnamed bitfield is legal and common: "int :3;" is padding and
      // "int :0;" forces the next member to a fresh storage unit.
      char *name = NULL;
      Type *mty = base;
      if (!peek(":")) {
        mty = declarator(base, &name);
        if (mty->kind == TY_VOID) error_at(tk->loc, "member declared void");
        if (mty->size < 0) error_at(tk->loc, "member has incomplete type");
      }

      Member *m = calloc(1, sizeof(Member));
      m->name = name;
      m->ty = mty;

      if (consume(":")) {
        if (!is_integer(mty))
          error_at(tk->loc, "a bitfield must have an integer type");
        m->is_bitfield = true;
        m->bit_width = (int)const_expr();
        if (m->bit_width < 0 || m->bit_width > mty->size * 8)
          error_at(tk->loc, "a %d-bit field does not fit in %d bits",
                   m->bit_width, mty->size * 8);
        if (m->bit_width == 0 && name)
          error_at(tk->loc, "a zero-width bitfield cannot have a name");
      } else if (!name) {
        error_at(tk->loc, "a member needs a name unless it is a bitfield");
      }

      cur = cur->next = m;
    }
  }
  ty->members = head.next;

  // Layout. A union overlays its members at offset 0 and takes the widest size;
  // a struct packs them in order with each one aligned, then rounds the total up
  // to the struct's own alignment so arrays of it stay aligned.
  int offset = 0;
  int bits = 0;        // bits used so far in the current storage unit
  for (Member *m = ty->members; m; m = m->next) {
    if (is_union) {
      m->offset = 0;
      if (m->is_bitfield) { m->bit_offset = 0; }
      if (ty->size < m->ty->size) ty->size = m->ty->size;
    } else if (m->is_bitfield && m->bit_width == 0) {
      // ":0" has no storage and no name. Its only job is to end the current
      // unit, so whatever comes next starts at a fresh boundary.
      bits = align_to(bits, m->ty->size * 8);
    } else if (m->is_bitfield) {
      // Keep packing into the unit already open if the field fits whole.
      // Straddling a boundary is allowed by the ABI but never required, and
      // gcc does not do it either, so a field that would straddle starts a
      // new unit instead.
      int unit = m->ty->size * 8;
      if (bits % unit + m->bit_width > unit)
        bits = align_to(bits, unit);
      m->offset = (bits / unit) * m->ty->size;
      m->bit_offset = bits % unit;
      bits += m->bit_width;
    } else {
      // An ordinary member closes whatever unit was open.
      offset = align_to((bits + 7) / 8, m->ty->align);
      m->offset = offset;
      offset += m->ty->size;
      bits = offset * 8;
    }
    if (ty->align < m->ty->align) ty->align = m->ty->align;
  }
  if (!is_union) ty->size = (bits + 7) / 8;
  ty->size = align_to(ty->size, ty->align);

  if (tag) {
    TagScope *here = find_tag_here(tag);
    // Completing a forward declaration must update the type the earlier
    // references already point at, so copy into it rather than replacing it.
    if (here) *here->ty = *ty;
    else push_tag(strndup(tag->loc, tag->len), ty);
  }
  return ty;
}

// enum-specifier = "enum" ident? ("{" ident ("=" const)? ("," ...)* "}")?
static Type *enum_decl(void) {
  expect("enum");
  Token *tag = NULL;
  if (tk->kind == TK_IDENT) { tag = tk; tk = tk->next; }

  if (tag && !peek("{")) {
    Type *known = find_tag(tag);
    if (!known) error_at(tag->loc, "unknown enum tag");
    return known;
  }

  expect("{");
  Type *ty = new_enum_type();
  long val = 0;
  bool first = true;
  while (!consume("}")) {
    if (!first) expect(",");
    first = false;
    if (consume("}")) break;                  // trailing comma
    char *name = get_ident();
    if (consume("=")) val = const_expr();
    VarScope *vs = push_scope(name);
    vs->enum_ty = ty;
    vs->enum_val = (int)val++;
  }

  if (tag) push_tag(strndup(tag->loc, tag->len), ty);
  return ty;
}

static Member *get_member(Type *ty, Token *t) {
  for (Member *m = ty->members; m; m = m->next)
    if (name_is(m->name, t)) return m;
  error_at(t->loc, "no member named '%.*s'", t->len, t->loc);
  return NULL;
}

static Node *as_stmt(Node *lhs, Node *rhs, Token *tok) {
  Node *a = new_binary(ND_ASSIGN, lhs, rhs, tok);
  add_type(a);
  return new_unary(ND_EXPR_STMT, a, tok);
}

// a[i], built as *(a+i) so the existing pointer arithmetic does the scaling.
static Node *elem_of(Node *base, int i, Token *tok) {
  Node *n = new_unary(ND_DEREF, new_add(base, new_num(i, tok), tok), tok);
  add_type(n);
  return n;
}

static Node *member_of(Node *base, Member *m, Token *tok) {
  Node *n = new_unary(ND_MEMBER, base, tok);
  n->member = m;
  n->ty = m->ty;
  return n;
}

// C zero-initialises whatever an initialiser list leaves out, which has to be
// done explicitly here because locals live on an uninitialised stack.
static void zero_walk(Node *base, Type *ty, Node **cur, Token *tok) {
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++)
      zero_walk(elem_of(base, i, tok), ty->base, cur, tok);
    return;
  }
  if (is_aggregate(ty)) {
    for (Member *m = ty->members; m; m = m->next) {
      zero_walk(member_of(base, m, tok), m->ty, cur, tok);
      if (ty->kind == TY_UNION) break;      // one member covers the storage
    }
    return;
  }
  *cur = (*cur)->next = as_stmt(base, new_num(0, tok), tok);
}

// Counts top-level elements of a brace initialiser without consuming it, so an
// unsized array can learn its own length.
static int count_init_elems(void) {
  Token *save = tk;
  if (!peek("{")) error_at(tk->loc, "expected an initialiser list");
  tk = tk->next;

  bool empty = peek("}");
  int depth = 0, commas = 0;
  Token *prev = NULL;
  while (!(depth == 0 && peek("}"))) {
    if (tk->kind == TK_EOF) error_at(save->loc, "unterminated initialiser");
    if (tok_is(tk, "{")) depth++;
    else if (tok_is(tk, "}")) depth--;
    else if (depth == 0 && tok_is(tk, ",")) commas++;
    prev = tk;
    tk = tk->next;
  }
  tk = save;
  if (empty) return 0;
  // A trailing comma is legal and does not introduce an element.
  return (prev && tok_is(prev, ",")) ? commas : commas + 1;
}

// Emits the statements that initialise the lvalue `base` of type `ty`.
static void init_walk(Node *base, Type *ty, Node **cur, Token *tok) {
  // char s[] = "abc";  copies the bytes including the terminating NUL.
  if (ty->kind == TY_ARRAY && ty->base->size == 1 && tk->kind == TK_STR) {
    Token *str = tk;
    tk = tk->next;
    for (int i = 0; i < ty->array_len; i++) {
      int ch = (i < str->str_len) ? (unsigned char)str->str[i] : 0;
      *cur = (*cur)->next = as_stmt(elem_of(base, i, tok), new_num(ch, tok), tok);
    }
    return;
  }

  if (ty->kind == TY_ARRAY) {
    expect("{");
    int i = 0;
    bool first = true;
    while (!peek("}")) {
      if (!first) expect(",");
      first = false;
      if (peek("}")) break;                       // trailing comma
      if (i >= ty->array_len) error_at(tk->loc, "too many initialisers for this array");
      init_walk(elem_of(base, i, tok), ty->base, cur, tok);
      i++;
    }
    expect("}");
    for (; i < ty->array_len; i++)
      zero_walk(elem_of(base, i, tok), ty->base, cur, tok);
    return;
  }

  if (is_aggregate(ty)) {
    // "struct P a = f();" initialises from a value rather than a list. The
    // brace form was the only one handled, so a struct-returning call could
    // be assigned but not used to initialise -- a distinction C does not
    // make and nothing would expect.
    if (!peek("{")) {
      Node *rhs = assign();
      add_type(rhs);
      if (!is_aggregate(rhs->ty) || rhs->ty->size != ty->size)
        error_at(tok->loc, "cannot initialise this struct or union from that value");
      *cur = (*cur)->next = as_stmt(base, rhs, tok);
      return;
    }
    expect("{");
    Member *m = ty->members;
    bool first = true;
    while (!peek("}")) {
      if (!first) expect(",");
      first = false;
      if (peek("}")) break;
      if (!m) error_at(tk->loc, "too many initialisers for this struct");
      init_walk(member_of(base, m, tok), m->ty, cur, tok);
      m = m->next;
      if (ty->kind == TY_UNION) break;            // only the first member
    }
    expect("}");
    for (; m; m = m->next) {
      zero_walk(member_of(base, m, tok), m->ty, cur, tok);
      if (ty->kind == TY_UNION) break;
    }
    return;
  }

  // Scalar. A brace around a scalar is legal: int x = {1};
  if (consume("{")) {
    *cur = (*cur)->next = as_stmt(base, assign(), tok);
    consume(",");
    expect("}");
    return;
  }
  *cur = (*cur)->next = as_stmt(base, assign(), tok);
}

// Two statics called "n" in two functions are two objects, and both want the
// label "n". The counter is the whole mechanism; gcc spells it n.0, n.1 and
// the dot is deliberate, since no C identifier can contain one and so nothing
// in the source can ever collide with a generated name.
static char *static_local_name(char *name) {
  static int seq = 0;
  char *s = malloc(strlen(name) + 16);
  sprintf(s, "%s.%d", name, seq++);
  return s;
}

// ---- initialisers for objects in static storage --------------------------
//
// A local initialiser is lowered to assignments and run; a global one has to
// exist before main does, so it is a byte pattern in the object file. Same
// surface syntax, completely different mechanism, which is why this does not
// share code with init_walk above.

static void write_le(char *buf, long val, int size) {
  for (int i = 0; i < size; i++)
    buf[i] = (char)((unsigned long)val >> (i * 8));
}

static Relocation *add_rel(Relocation **list, int offset, char *label, long addend) {
  Relocation *r = calloc(1, sizeof(Relocation));
  r->offset = offset; r->label = label; r->addend = addend;
  // Kept in increasing offset order so emission is one linear pass.
  Relocation **p = list;
  while (*p && (*p)->offset < offset) p = &(*p)->next;
  r->next = *p;
  *p = r;
  return r;
}

// Fill buf[offset .. offset+ty->size) from the initialiser at the cursor.
static void global_init(char *buf, Relocation **rel, Type *ty, int offset) {
  if (ty->kind == TY_ARRAY) {
    // "char s[] = \"abc\";" copies the bytes, rather than storing a pointer
    // to them: the array IS the storage.
    if (ty->base->kind == TY_CHAR && tk->kind == TK_STR) {
      Token *t = tk;
      tk = tk->next;
      int n = t->str_len < ty->size ? t->str_len : ty->size;
      memcpy(buf + offset, t->str, n);
      return;
    }
    expect("{");
    for (int i = 0; i < ty->array_len; i++) {
      if (i > 0 && !consume(",")) break;
      if (peek("}")) break;
      global_init(buf, rel, ty->base, offset + i * ty->base->size);
    }
    consume(",");
    expect("}");
    return;
  }

  if (is_aggregate(ty)) {
    expect("{");
    Member *m = ty->members;
    bool first = true;
    while (!peek("}")) {
      if (!first && !consume(",")) break;
      if (peek("}")) break;
      first = false;

      // A designator names the member to initialise, and the ones it skips
      // keep the zero the buffer already holds. Members after a designator
      // continue from it, which is what makes ".size = 4, .align = 4" work
      // without naming every field in between.
      if (consume(".")) {
        if (tk->kind != TK_IDENT) error_at(tk->loc, "expected a member name after '.'");
        m = get_member(ty, tk);
        tk = tk->next;
        expect("=");
      }
      if (!m) error_at(tk->loc, "too many initialisers for this struct");

      global_init(buf, rel, m->ty, offset + m->offset);
      if (ty->kind == TY_UNION) break;   // only one member has storage
      m = m->next;
    }
    consume(",");
    expect("}");
    return;
  }

  // A braced scalar is legal: "int x = {5};".
  bool braced = consume("{");
  Node *n = assign();
  add_type(n);
  if (braced) { consume(","); expect("}"); }

  char *label = NULL;
  long val = eval_addr(n, &label);
  if (label) {
    if (ty->size != 8)
      error_at(n->tok->loc, "an address does not fit in %d bytes", ty->size);
    add_rel(rel, offset, label, val);
    return;     // the bytes stay zero; the linker writes them
  }
  write_le(buf + offset, val, ty->size);
}

// Parse "= ..." for an object in static storage and attach the result.
static void read_static_init(Obj *var) {
  var->init_data = calloc(1, var->ty->size ? var->ty->size : 1);
  var->init_len = var->ty->size;
  global_init(var->init_data, &var->rel, var->ty, 0);
}

// declaration = declspec declarator ("=" initialiser)? ("," ...)* ";"
static Node *declaration(void) {
  VarAttr attr = {0};
  Type *base = declspec(&attr);
  Node head = {0};
  Node *cur = &head;
  bool first = true;

  while (!consume(";")) {
    if (!first) expect(",");
    first = false;
    char *name;
    Type *ty = declarator(base, &name);

    // A typedef declares a name, not an object: register it and emit nothing.
    if (attr.is_typedef) {
      push_scope(name)->type_def = ty;
      continue;
    }

    if (ty->kind == TY_VOID) error_at(tk->loc, "variable declared void");

    // An unsized array learns its length from the initialiser, so the length has
    // to be settled before the Obj is created and given a stack slot.
    if (ty->kind == TY_ARRAY && ty->array_len < 0) {
      if (!peek("=")) error_at(tk->loc, "array size missing and no initialiser to infer it from");
      Token *eq = tk;
      tk = tk->next;
      int n;
      if (tk->kind == TK_STR) n = tk->str_len;        // includes the NUL
      else n = count_init_elems();
      ty = array_of(ty->base, n);
      tk = eq;                                        // put '=' back
    }

    if (ty->size < 0) error_at(tk->loc, "variable has incomplete type");

    // A static local has block scope and static storage: the name is visible
    // only here, but the object outlives the call and keeps its value. So it
    // is a global that happens to be announced inside a function, and the
    // only real work is giving it a name the assembler will not collide with
    // some other function's static of the same name.
    if (attr.is_static) {
      if (peek("=") && ty->kind == TY_ARRAY && ty->array_len < 0) {
        Token *eq = tk;
        tk = tk->next;
        int n = (tk->kind == TK_STR) ? tk->str_len : count_init_elems();
        ty = array_of(ty->base, n);
        tk = eq;
      }
      Obj *var = new_var(static_local_name(name), ty, false);
      var->is_static = true;
      // new_var pushed the mangled name; the source name is what has to
      // resolve inside this block.
      push_scope(name)->var = var;
      if (consume("=")) read_static_init(var);
      continue;
    }

    Obj *var = new_var(name, ty, true);

    if (consume("=")) {
      Token *tok = tk;
      init_walk(new_var_node(var, tok), ty, &cur, tok);
    }
  }

  Node *blk = new_node(ND_BLOCK, tk);
  blk->body = head.next;
  return blk;
}

// Pointer arithmetic lives here: p + n scales n by sizeof(*p).
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs); add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);
  if (is_pointer(lhs->ty) && is_pointer(rhs->ty))
    error_at(tok->loc, "invalid operands to +");
  if (is_integer(lhs->ty) && is_pointer(rhs->ty)) { Node *t = lhs; lhs = rhs; rhs = t; }

  int sz = lhs->ty->base->size;
  // The index is widened to long BEFORE it is scaled. Both operands are
  // otherwise int-sized, so the multiply is emitted as a 32-bit imul, whose
  // result is zero-extended into the full register: index -1 becomes
  // 0xFFFFFFFF and "p[-1]" reads four gigabytes forward instead of one byte
  // back. An index is a pointer offset and belongs in 64 bits from the start.
  Node *scaled = new_binary(ND_MUL, new_cast(rhs, ty_long), new_num(sz, tok), tok);
  add_type(scaled);
  Node *n = new_binary(ND_ADD, lhs, scaled, tok);
  // Decay: pointer arithmetic on an array yields a pointer to its element.
  n->ty = pointer_to(lhs->ty->base);
  return n;
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs); add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);

  if (is_pointer(lhs->ty) && is_integer(rhs->ty)) {
    int sz = lhs->ty->base->size;
    Node *scaled = new_binary(ND_MUL, new_cast(rhs, ty_long), new_num(sz, tok), tok);
    add_type(scaled);
    Node *n = new_binary(ND_SUB, lhs, scaled, tok);
    n->ty = pointer_to(lhs->ty->base);
    return n;
  }

  if (is_pointer(lhs->ty) && is_pointer(rhs->ty)) {
    // ptr - ptr is an element count, not a byte count.
    int sz = lhs->ty->base->size;
    Node *diff = new_binary(ND_SUB, lhs, rhs, tok);
    diff->ty = ty_long;
    Node *n = new_binary(ND_DIV, diff, new_num(sz, tok), tok);
    n->ty = ty_long;
    return n;
  }

  error_at(tok->loc, "invalid operands to -");
  return NULL;
}

// A call through a pointer. The parameter types are unknown here -- the
// pointer's type records only that it points at a function -- so arguments
// are passed as written, which is what C says happens when no prototype is
// in scope at the call.
static Node *indirect_call(Node *callee, Token *tok) {
  expect("(");
  Node head = {0};
  Node *cur = &head;
  while (!consume(")")) {
    if (cur != &head) expect(",");
    cur = cur->next = assign();
    add_type(cur);
  }
  Node *n = new_node(ND_FUNCALL, tok);
  n->callee = callee;
  n->args = head.next;
  // The call's type is the pointed-at function's return type.
  Type *ft = callee->ty->kind == TY_PTR ? callee->ty->base : callee->ty;
  n->ty = ft->ret ? ft->ret : ty_long;
  return n;
}

static Node *funcall(char *name, Token *tok) {
  // The declaration, if this call has one. Looked up before the arguments are
  // parsed so each one can be converted to its parameter's type on the way in.
  VarScope *vs = find_name(tok);
  Obj *fn = (vs && vs->var && vs->var->is_function) ? vs->var : NULL;
  Obj *param = fn ? fn->params : NULL;

  Node head = {0};
  Node *cur = &head;
  int nargs = 0;

  while (!consume(")")) {
    if (cur != &head) expect(",");
    Token *atok = tk;
    Node *arg = assign();
    add_type(arg);

    if (param) {
      if (is_aggregate(param->ty) && !aggregate_regs(param->ty))
        error_at(atok->loc, "passing a %d-byte struct or union by value is not "
                            "supported (the MEMORY class needs stack arguments; "
                            "see PLAN.md)", param->ty->size);
      // Converted as if by assignment, which is the whole point of having the
      // prototype: f(char) called with an int must narrow at the call, not
      // inside the callee where the value has already arrived wrong.
      //
      // An aggregate is exempt. There is no conversion to apply -- the types
      // have to match already -- and wrapping it in a cast turns the
      // argument into a non-lvalue, which is a problem because passing a
      // struct by value starts by taking its address.
      if (!is_aggregate(param->ty)) arg = new_cast(arg, param->ty);
      param = param->next;
    } else if (fn && fn->has_proto && !fn->is_variadic) {
      error_at(atok->loc, "too many arguments to '%s'", name);
    }

    cur = cur->next = arg;
    nargs++;
  }

  // "f()" with empty parens declares nothing about its arguments, so only a
  // real prototype is allowed to complain about the count.
  if (fn && fn->has_proto && param)
    error_at(tok->loc, "too few arguments to '%s'", name);

  Node *n = new_node(ND_FUNCALL, tok);
  n->funcname = name;
  n->args = head.next;
  n->fn = fn;
  (void)nargs;

  Type *ret = fn ? fn->ty->ret : NULL;
  if (ret && is_aggregate(ret)) {
    if (!aggregate_regs(ret))
      error_at(tok->loc, "a call returning a %d-byte struct or union is not "
                         "supported (the MEMORY class needs the hidden-pointer "
                         "protocol; see PLAN.md)", ret->size);
    n->ret_buf = new_var("", ret, true);
  }
  return n;
}

// primary = "(" expr ")" | "sizeof" unary | ident ("(" args ")")? | num | str
static Node *unary(void);
static Node *postfix(void);
static Node *cast_expr(void);

// abstract-declarator, i.e. a type with no identifier attached.
static Type *typename_(void) {
  Type *ty = declspec(NULL);
  skip_qualifiers();
  while (consume("*")) { ty = pointer_to(ty); skip_qualifiers(); }
  while (peek("[")) ty = type_suffix(ty);
  return ty;
}

static Node *primary(void) {
  Token *tok = tk;

  if (consume("(")) {
    Node *n = expr();
    expect(")");
    return n;
  }

  if (consume("sizeof")) {
    // sizeof(type) vs sizeof expr. Only a typename after '(' means the former,
    // and "typename" now includes typedefs, so this needs the scope stack.
    if (peek("(")) {
      Token *save = tk;
      tk = tk->next;
      if (is_typename()) {
        Type *ty = typename_();
        expect(")");
        if (ty->size < 0) error_at(tok->loc, "sizeof applied to an incomplete type");
        return new_num(ty->size, tok);
      }
      tk = save;
    }
    Node *n = unary();
    add_type(n);
    if (n->ty->size < 0) error_at(tok->loc, "sizeof applied to an incomplete type");
    return new_num(n->ty->size, tok);
  }

  if (tk->kind == TK_NUM) {
    Node *n = new_num(tk->val, tk);
    if (tk->num_long)
      n->ty = tk->num_unsigned ? ty_ulong : ty_long;
    else if (tk->num_unsigned)
      n->ty = ty_uint;
    tk = tk->next;
    return n;
  }

  if (tk->kind == TK_STR) {
    Obj *v = new_string_literal(tk);
    tk = tk->next;
    return new_var_node(v, tok);
  }

  if (tk->kind == TK_IDENT) {
    // The two builtins. They are not calls: va_start writes four fields of a
    // struct the caller owns, and va_arg's second operand is a TYPE, which
    // no function signature can describe.
    if (tok_is(tk, "__builtin_va_start")) {
      tk = tk->next;
      expect("(");
      Node *n = new_node(ND_VA_START, tok);
      n->lhs = assign();
      add_type(n->lhs);
      // The second argument names the last fixed parameter. Its only job is
      // to say how many register slots are already spoken for, and that is
      // known from the declaration, so the expression is parsed and dropped.
      if (consume(",")) { assign(); }
      expect(")");
      n->ty = ty_void;
      return n;
    }
    if (tok_is(tk, "__builtin_va_arg")) {
      tk = tk->next;
      expect("(");
      Node *n = new_node(ND_VA_ARG, tok);
      n->lhs = assign();
      add_type(n->lhs);
      expect(",");
      Type *ty = typename_();
      expect(")");
      if (is_aggregate(ty))
        error_at(tok->loc, "va_arg of a struct or union is not supported "
                           "(needs SysV eightbyte classification; see PLAN.md)");
      n->ty = ty;
      return n;
    }

    char *name = get_ident();
    // Only a name that is not a variable becomes a direct call. If it names
    // a function pointer, the value has to be loaded and postfix() makes the
    // call -- otherwise "fp(3)" assembles into "call fp", a direct jump to
    // the address of the pointer rather than through it, and the linker
    // happily resolves the symbol.
    if (peek("(")) {
      VarScope *v = find_name(tok);
      if (!v || !v->var || v->var->is_function) {
        tk = tk->next;                  // funcall starts after the '('
        return funcall(name, tok);
      }
    }

    VarScope *vs = find_name(tok);
    if (!vs) error_at(tok->loc, "undefined identifier '%s'", name);
    // An enum constant is just a number that happened to have a name.
    if (vs->enum_ty) return new_num(vs->enum_val, tok);
    if (!vs->var) error_at(tok->loc, "'%s' is a type, not a value", name);
    return new_var_node(vs->var, tok);
  }

  error_at(tk->loc, "expected an expression");
  return NULL;
}

// postfix = primary ("(" args ")" | "[" expr "]" | "." ident | "->" ident | "++" | "--")*
static Node *postfix(void) {
  Node *n = primary();
  for (;;) {
    Token *tok = tk;
    // A call is an operator on an expression, not a thing only a name can
    // do. Keeping it in primary() meant "m->builtin(tok)" could not be
    // written, and that is the shape cc.c uses for its own builtin macros.
    if (peek("(")) {
      add_type(n);
      if (n->ty && (n->ty->kind == TY_FUNC ||
                    (n->ty->kind == TY_PTR && n->ty->base->kind == TY_FUNC))) {
        n = indirect_call(n, tok);
        continue;
      }
    }
    if (consume("[")) {               // a[i] is *(a+i), exactly as C says
      Node *idx = expr();
      expect("]");
      n = new_unary(ND_DEREF, new_add(n, idx, tok), tok);
      add_type(n);
      continue;
    }
    if (consume(".")) {
      add_type(n);
      if (!is_aggregate(n->ty)) error_at(tok->loc, "'.' applied to something that is not a struct or union");
      Node *m = new_unary(ND_MEMBER, n, tok);
      m->member = get_member(n->ty, tk);
      tk = tk->next;
      m->ty = m->member->ty;
      n = m;
      continue;
    }
    if (consume("->")) {
      // x->y is exactly (*x).y, so build it that way instead of duplicating.
      n = new_unary(ND_DEREF, n, tok);
      add_type(n);
      if (!is_aggregate(n->ty)) error_at(tok->loc, "'->' applied to something that is not a pointer to struct or union");
      Node *m = new_unary(ND_MEMBER, n, tok);
      m->member = get_member(n->ty, tk);
      tk = tk->next;
      m->ty = m->member->ty;
      n = m;
      continue;
    }
    if (consume("++") || consume("--")) {
      bool inc = tok_is(tok, "++");
      // i++  ==>  (i = i + 1) - 1   : value before the bump
      add_type(n);
      Node *one = new_num(1, tok);
      Node *bumped = inc ? new_add(n, one, tok) : new_sub(n, one, tok);
      Node *as = new_binary(ND_ASSIGN, n, bumped, tok);
      add_type(as);
      n = inc ? new_sub(as, new_num(1, tok), tok) : new_add(as, new_num(1, tok), tok);
      add_type(n);
      continue;
    }
    return n;
  }
}

// unary = ("+" | "-" | "*" | "&" | "!" | "~" | "++" | "--") unary | postfix
static Node *unary(void) {
  Token *tok = tk;
  if (consume("+")) return cast_expr();
  if (consume("-")) { Node *n = new_unary(ND_NEG, cast_expr(), tok); add_type(n); return n; }
  if (consume("*")) { Node *n = new_unary(ND_DEREF, cast_expr(), tok); add_type(n); return n; }
  if (consume("&")) { Node *n = new_unary(ND_ADDR, cast_expr(), tok); add_type(n); return n; }
  if (consume("!")) { Node *n = new_unary(ND_NOT, cast_expr(), tok); add_type(n); return n; }
  if (consume("~")) { Node *n = new_unary(ND_BITNOT, cast_expr(), tok); add_type(n); return n; }
  if (consume("++")) {                       // ++i is i = i + 1
    Node *n = unary();
    Node *a = new_binary(ND_ASSIGN, n, new_add(n, new_num(1, tok), tok), tok);
    add_type(a); return a;
  }
  if (consume("--")) {
    Node *n = unary();
    Node *a = new_binary(ND_ASSIGN, n, new_sub(n, new_num(1, tok), tok), tok);
    add_type(a); return a;
  }
  return postfix();
}

// cast = "(" type-name ")" cast | unary
//
// The "(" is ambiguous with a parenthesised expression, and only the scope stack
// can resolve it: "(x)" is a cast if and only if x names a type here.
// An unnamed object with an initialiser, living as long as the scope it
// appears in. At file scope that is forever, so it becomes an anonymous
// global with a byte pattern -- which is how
//   #define PRIM(k, sz, uns) &(Type){.kind = (k), ...}
// gives a static initialiser something to take the address of.
//
// Inside a function it would be an anonymous local with the initialiser
// lowered to assignments; nothing here needs that yet, so it is refused by
// name rather than quietly given static storage, which would make two calls
// share one object.
static int literal_id;
// The flag is declared near the top: not `locals != NULL`, which is
// whatever the last function or prototype left behind, so every file-scope
// initialiser after the first prototype would look like it is inside one.
static Node *compound_literal(Type *ty, Token *tok) {
  if (in_function_body)
    error_at(tok->loc, "a compound literal inside a function is not supported yet "
                       "(it would need an anonymous local; see PLAN.md)");

  char name[32];
  snprintf(name, sizeof(name), ".L.lit.%d", literal_id++);
  Obj *v = new_var(strdup(name), ty, false);
  v->is_static = true;
  read_static_init(v);
  return new_var_node(v, tok);
}

static Node *cast_expr(void) {
  if (peek("(")) {
    Token *save = tk;
    tk = tk->next;
    if (is_typename()) {
      Type *ty = typename_();
      expect(")");
      // "(Type){...}" is a compound literal, not a cast: the parenthesised
      // type names an unnamed OBJECT rather than a conversion. The two are
      // distinguished only by what follows the paren.
      if (peek("{")) return compound_literal(ty, save);
      Node *n = new_cast(cast_expr(), ty);
      return n;
    }
    tk = save;
  }
  return unary();
}

static Node *mul(void) {
  Node *n = cast_expr();
  for (;;) {
    Token *tok = tk;
    if (consume("*"))      { n = new_binary(ND_MUL, n, cast_expr(), tok); add_type(n); }
    else if (consume("/")) { n = new_binary(ND_DIV, n, cast_expr(), tok); add_type(n); }
    else if (consume("%")) { n = new_binary(ND_MOD, n, cast_expr(), tok); add_type(n); }
    else return n;
  }
}

static Node *add(void) {
  Node *n = mul();
  for (;;) {
    Token *tok = tk;
    if (consume("+"))      n = new_add(n, mul(), tok);
    else if (consume("-")) n = new_sub(n, mul(), tok);
    else return n;
  }
}

static Node *shift(void) {
  Node *n = add();
  for (;;) {
    Token *tok = tk;
    if (consume("<<"))      { n = new_binary(ND_SHL, n, add(), tok); add_type(n); }
    else if (consume(">>")) { n = new_binary(ND_SHR, n, add(), tok); add_type(n); }
    else return n;
  }
}

static Node *relational(void) {
  Node *n = shift();
  for (;;) {
    Token *tok = tk;
    // a > b is b < a; a >= b is b <= a. One codegen path instead of four.
    if (consume("<"))       { n = new_binary(ND_LT, n, shift(), tok); add_type(n); }
    else if (consume("<=")) { n = new_binary(ND_LE, n, shift(), tok); add_type(n); }
    else if (consume(">"))  { n = new_binary(ND_LT, shift(), n, tok); add_type(n); }
    else if (consume(">=")) { n = new_binary(ND_LE, shift(), n, tok); add_type(n); }
    else return n;
  }
}

static Node *equality(void) {
  Node *n = relational();
  for (;;) {
    Token *tok = tk;
    if (consume("=="))      { n = new_binary(ND_EQ, n, relational(), tok); add_type(n); }
    else if (consume("!=")) { n = new_binary(ND_NE, n, relational(), tok); add_type(n); }
    else return n;
  }
}

static Node *bitand_(void) {
  Node *n = equality();
  Token *tok;
  while ((tok = tk), consume("&")) { n = new_binary(ND_BITAND, n, equality(), tok); add_type(n); }
  return n;
}
static Node *bitxor_(void) {
  Node *n = bitand_();
  Token *tok;
  while ((tok = tk), consume("^")) { n = new_binary(ND_BITXOR, n, bitand_(), tok); add_type(n); }
  return n;
}
static Node *bitor_(void) {
  Node *n = bitxor_();
  Token *tok;
  while ((tok = tk), consume("|")) { n = new_binary(ND_BITOR, n, bitxor_(), tok); add_type(n); }
  return n;
}
static Node *logand(void) {
  Node *n = bitor_();
  Token *tok;
  while ((tok = tk), consume("&&")) { n = new_binary(ND_AND, n, bitor_(), tok); add_type(n); }
  return n;
}
static Node *logor(void) {
  Node *n = logand();
  Token *tok;
  while ((tok = tk), consume("||")) { n = new_binary(ND_OR, n, logand(), tok); add_type(n); }
  return n;
}

// conditional = logor ("?" expr ":" conditional)?
static Node *conditional(void) {
  Node *c = logor();
  if (!peek("?")) return c;
  Token *tok = tk;
  tk = tk->next;
  Node *n = new_node(ND_COND, tok);
  n->cond = c;
  n->then = expr();
  expect(":");
  n->els = conditional();
  add_type(n);
  return n;
}

// assign = conditional (("=" | "+=" | ...) assign)?
static Node *assign(void) {
  Node *n = conditional();
  Token *tok = tk;

  if (consume("=")) { n = new_binary(ND_ASSIGN, n, assign(), tok); add_type(n); return n; }

  // Compound assignment. a += b becomes a = a + b. This evaluates the lvalue
  // twice, which is wrong for side-effecting lvalues like *p++ += 1.
  // shortcut: no temporaries yet. Fix with a hidden temp when a real program
  // trips it; nothing in t/ does.
  struct { char *op; NodeKind kind; } ops[] = {
    {"+=", ND_ADD}, {"-=", ND_SUB}, {"*=", ND_MUL}, {"/=", ND_DIV}, {"%=", ND_MOD},
    {"&=", ND_BITAND}, {"|=", ND_BITOR}, {"^=", ND_BITXOR},
    {"<<=", ND_SHL}, {">>=", ND_SHR}, {NULL, 0},
  };
  for (int i = 0; ops[i].op; i++) {
    if (!consume(ops[i].op)) continue;
    Node *rhs = assign();
    Node *combined;
    if (ops[i].kind == ND_ADD)      combined = new_add(n, rhs, tok);
    else if (ops[i].kind == ND_SUB) combined = new_sub(n, rhs, tok);
    else { combined = new_binary(ops[i].kind, n, rhs, tok); add_type(combined); }
    n = new_binary(ND_ASSIGN, n, combined, tok);
    add_type(n);
    return n;
  }
  return n;
}

static Node *expr(void) {
  Node *n = assign();
  Token *tok = tk;
  if (consume(",")) { n = new_binary(ND_COMMA, n, expr(), tok); add_type(n); }
  return n;
}

static Node *expr_stmt(void) {
  Token *tok = tk;
  if (consume(";")) return new_node(ND_BLOCK, tok);   // empty statement
  Node *n = new_unary(ND_EXPR_STMT, expr(), tok);
  add_type(n);
  expect(";");
  return n;
}

static Node *stmt(void) {
  Token *tok = tk;

  if (consume("return")) {
    // "return;" is how every void function ends early, and nothing in this
    // suite had one until the compiler was pointed at its own source.
    if (consume(";")) {
      Node *n = new_node(ND_RETURN, tok);
      n->ty = ty_void;
      return n;
    }
    Node *n = new_unary(ND_RETURN, expr(), tok);
    add_type(n);
    expect(";");
    return n;
  }

  if (consume("if")) {
    Node *n = new_node(ND_IF, tok);
    expect("("); n->cond = expr(); expect(")");
    add_type(n->cond);
    n->then = stmt();
    if (consume("else")) n->els = stmt();
    return n;
  }

  if (consume("for")) {
    Node *n = new_node(ND_FOR, tok);
    expect("(");
    // "for (int i = 0; ...)" declares i in a scope of its own, so the next loop
    // in the same block can declare i again.
    enter_scope();
    char *sb = brk_label, *sc = cont_label;
    brk_label = cont_label = NULL;   // assigned in codegen

    if (!peek(";")) n->init = is_typename() ? declaration() : expr_stmt();
    else { tk = tk->next; }
    if (!peek(";")) { n->cond = expr(); add_type(n->cond); }
    expect(";");
    if (!peek(")")) { n->inc = expr(); add_type(n->inc); }
    expect(")");
    n->then = stmt();

    leave_scope();
    brk_label = sb; cont_label = sc;
    return n;
  }

  if (consume("while")) {
    Node *n = new_node(ND_FOR, tok);   // while is for without init/inc
    expect("("); n->cond = expr(); expect(")");
    add_type(n->cond);
    n->then = stmt();
    return n;
  }

  if (consume("do")) {
    Node *n = new_node(ND_DO, tok);
    n->then = stmt();
    expect("while"); expect("("); n->cond = expr(); expect(")"); expect(";");
    add_type(n->cond);
    return n;
  }

  if (consume("switch")) {
    Node *n = new_node(ND_SWITCH, tok);
    expect("("); n->cond = expr(); expect(")");
    add_type(n->cond);
    if (!is_integer(n->cond->ty)) error_at(tok->loc, "switch requires an integer");

    // case and default attach themselves to the innermost switch, and break
    // inside a switch means "leave the switch", not "leave the enclosing loop".
    Node *outer = current_switch;
    char *sb = brk_label;
    current_switch = n;
    brk_label = NULL;            // real label assigned during codegen
    n->then = stmt();
    current_switch = outer;
    brk_label = sb;
    return n;
  }

  if (consume("case")) {
    if (!current_switch) error_at(tok->loc, "case outside of a switch");
    Node *n = new_node(ND_CASE, tok);
    n->case_val = const_expr();
    expect(":");
    n->lhs = stmt();
    // Prepended, so codegen emits the comparisons in reverse source order. The
    // order of the tests does not matter; only that each value maps to one label.
    n->case_next = current_switch->case_next;
    current_switch->case_next = n;
    return n;
  }

  if (consume("default")) {
    if (!current_switch) error_at(tok->loc, "default outside of a switch");
    if (current_switch->default_case) error_at(tok->loc, "duplicate default");
    expect(":");
    Node *n = new_node(ND_CASE, tok);
    n->lhs = stmt();
    current_switch->default_case = n;
    return n;
  }

  if (consume("goto")) {
    Node *n = new_node(ND_GOTO, tok);
    n->label = get_ident();
    expect(";");
    n->goto_next = gotos;         // resolved against labels once the body is parsed
    gotos = n;
    return n;
  }

  // A labelled statement:  ident ":" stmt
  if (tk->kind == TK_IDENT && tok_is(tk->next, ":")) {
    Node *n = new_node(ND_LABEL, tok);
    n->label = strndup(tk->loc, tk->len);
    tk = tk->next; tk = tk->next;
    n->unique_label = calloc(1, 32);
    snprintf(n->unique_label, 32, ".L.user.%d", label_counter++);
    n->lhs = stmt();
    n->goto_next = labels;
    labels = n;
    return n;
  }

  if (consume("break"))    { expect(";"); return new_node(ND_BREAK, tok); }
  if (consume("continue")) { expect(";"); return new_node(ND_CONTINUE, tok); }

  if (consume("{")) return compound_stmt();

  return expr_stmt();
}

static Node *compound_stmt(void) {
  Token *tok = tk;
  Node head = {0};
  Node *cur = &head;

  enter_scope();
  while (!consume("}")) {
    // An identifier is a declaration only if it names a typedef in scope, so
    // this test depends on the scope stack rather than on the token alone.
    cur = cur->next = is_typename() ? declaration() : stmt();
    add_type(cur);
  }
  leave_scope();

  Node *n = new_node(ND_BLOCK, tok);
  n->body = head.next;
  return n;
}

static int align_to(int n, int a) { return (n + a - 1) / a * a; }

static void create_param_vars(Obj *params) {
  if (!params) return;
  create_param_vars(params->next);           // keep declaration order
  new_var(params->name, params->ty, true);
}

// function = declspec declarator "(" params ")" ("{" compound_stmt | ";")
static Obj *function(Type *base, VarAttr *attr) {
  char *name;
  Type *ty = base;
  skip_qualifiers();
  while (consume("*")) { ty = pointer_to(ty); skip_qualifiers(); }
  name = get_ident();
  expect("(");

  locals = NULL;

  // Collect parameter types first; we create the Obj's after so that their
  // stack offsets come out in declaration order.
  Obj phead = {0};
  Obj *pcur = &phead;
  bool variadic = false;
  // "f()" says nothing about its arguments and "f(void)" says there are none.
  // Both produce an empty parameter list, so the difference has to be noticed
  // here or it is lost: only the second one may be checked at a call.
  bool has_proto = !peek(")");
  if (peek("void") && tok_is(tk->next, ")")) tk = tk->next;   // f(void)
  bool pfirst = true;
  while (!consume(")")) {
    if (!pfirst) expect(",");
    pfirst = false;
    if (consume("...")) { expect(")"); variadic = true; break; }
    Type *pbase = declspec(NULL);
    // A parameter name is optional, and in a prototype it is usually absent:
    // "int f(int);" is the form almost every header uses. The type is what
    // matters at a call; the name only matters to a body.
    char *pname = NULL;
    Type *pty = declarator(pbase, &pname);
    if (pty->kind == TY_ARRAY) pty = pointer_to(pty->base);   // arrays decay
    if (pty->kind == TY_FUNC) pty = pointer_to(pty);          // so do functions
    Obj *p = calloc(1, sizeof(Obj));
    p->name = pname; p->ty = pty;
    pcur = pcur->next = p;
  }

  // A prototype followed by a definition is one function, not two. Creating a
  // second Obj would put a second symbol in the output and leave the call
  // sites bound to whichever happened to be found first.
  Obj *fn = NULL;
  for (Obj *g = globals; g; g = g->next)
    if (g->is_function && !strcmp(g->name, name)) { fn = g; break; }

  if (fn) {
    if (fn->ty->ret->kind != ty->kind || fn->ty->ret->is_unsigned != ty->is_unsigned)
      error_at(tk->loc, "'%s' redeclared with a different return type", name);
  } else {
    fn = calloc(1, sizeof(Obj));
    fn->name = name;
    fn->ty = func_returning(ty);
    fn->is_function = true;
    fn->next = globals;
    globals = fn;
    // Into the file scope, so a call can find it. This has to happen before
    // enter_scope() below, and before the body is parsed, or a recursive call
    // would not see its own function.
    push_scope(name)->var = fn;
  }
  fn->is_variadic = variadic;
  fn->has_proto = has_proto;
  // "static int f();" then "int f() {...}" keeps the internal linkage the
  // first declaration gave it, which is what C says and what the linker will
  // assume either way.
  if (attr && attr->is_static) fn->is_static = true;

  // Parameters and body share one scope, so a parameter can be shadowed by a
  // declaration in an inner block but not by one at the top level of the body.
  enter_scope();
  create_param_vars(phead.next);
  fn->params = locals;

  if (consume(";")) {           // prototype
    leave_scope();
    // Deliberately does NOT clear is_definition. A declaration says nothing
    // about whether this translation unit defines the function, and since
    // prototype and definition now share one Obj, clearing it here makes a
    // declaration that appears AFTER a definition delete the definition.
    // The symbol then vanishes from the output and only the linker notices.
    return fn;
  }

  // The struct-by-value refusal belongs here rather than at the declaration.
  // A prototype for a function taking a struct is harmless as long as nobody
  // calls it, and refusing it early costs real headers: glibc declares
  // fopencookie(void *, const char *, cookie_io_functions_t), and nothing
  // that merely includes <stdio.h> is asking to call it. A definition is
  // different, because this is where the argument would have to be received.
  for (Obj *pp = fn->params; pp; pp = pp->next)
    if (is_aggregate(pp->ty) && !aggregate_regs(pp->ty))
      error_at(tk->loc, "a %d-byte struct or union parameter is not supported "
                        "(the MEMORY class needs stack arguments; see PLAN.md)",
               pp->ty->size);
  if (is_aggregate(ty) && !aggregate_regs(ty))
    error_at(tk->loc, "returning a %d-byte struct or union is not supported "
                      "(the MEMORY class needs the hidden-pointer protocol; "
                      "see PLAN.md)", ty->size);

  expect("{");
  fn->is_definition = true;
  gotos = labels = NULL;
  in_function_body = true;
  fn->body = compound_stmt();
  in_function_body = false;
  fn->locals = locals;
  leave_scope();

  // A goto may refer to a label further down the function, so resolution has to
  // wait until the whole body is parsed. Labels are function-scoped, not
  // block-scoped, which is why they do not live in the Scope stack.
  for (Node *g = gotos; g; g = g->goto_next) {
    Node *target = NULL;
    for (Node *l = labels; l; l = l->goto_next)
      if (!strcmp(g->label, l->label)) { target = l; break; }
    if (!target) error_at(g->tok->loc, "use of undeclared label '%s'", g->label);
    g->unique_label = target->unique_label;
  }

  int off = 0;
  // A variadic function must be able to hand out arguments that arrived in
  // registers, and registers are not addressable. The ABI's answer is to
  // spill all six on entry into a known block and point va_list at it.
  // 48 bytes, no floats, so the fp half is never touched.
  if (fn->is_variadic) {
    off = 48;
    fn->va_area = -off;
  }
  for (Obj *v = fn->locals; v; v = v->next) {
    off += v->ty->size;
    off = align_to(off, v->ty->size > 8 ? 8 : v->ty->size);
    v->offset = -off;
  }
  fn->stack_size = align_to(off, 16);
  return fn;
}

static void global_variable(Type *base, VarAttr *attr) {
  bool first = true;
  while (!consume(";")) {
    if (!first) expect(",");
    first = false;
    char *name;
    Type *ty = declarator(base, &name);
    if (attr && attr->is_typedef) { push_scope(name)->type_def = ty; continue; }
    // "int a[] = {1,2,3};" is only incomplete until the initialiser is read,
    // so the length has to be settled before the type is judged and before
    // the Obj that will carry its storage exists.
    bool has_init = peek("=");
    if (has_init && ty->kind == TY_ARRAY && ty->array_len < 0) {
      Token *eq = tk;
      tk = tk->next;
      int n = (tk->kind == TK_STR) ? tk->str_len : count_init_elems();
      ty = array_of(ty->base, n);
      tk = eq;
    }

    // An extern declaration names something defined in another translation
    // unit, so its size may well be unknown here and emitting storage for it
    // would create a second definition at link time.
    if (!(attr && attr->is_extern) && ty->size < 0)
      error_at(tk->loc, "global has incomplete type");
    Obj *v = new_var(name, ty, false);
    if (attr) { v->is_static = attr->is_static; v->is_extern = attr->is_extern; }

    if (consume("=")) {
      // C11 6.9.2: a file-scope declaration with an initialiser IS a
      // definition, and the extern becomes a statement about linkage only.
      // Refusing this was the first thing refuse() caught in cc rather than
      // in a test, because the helper makes gcc agree before a refusal counts.
      v->is_extern = false;
      read_static_init(v);
    }
  }
}

// Distinguish "int f(" from "int x" by lookahead.
static bool is_function_decl(void) {
  if (peek(";")) return false;
  Token *save = tk;
  while (consume("*")) {}
  if (tk->kind != TK_IDENT) { tk = save; return false; }
  tk = tk->next;
  bool r = peek("(");
  tk = save;
  return r;
}

// The SysV x86-64 va_list, constructed rather than parsed, because there is
// no header to read it from and its layout is fixed by the ABI anyway:
//
//   struct { unsigned gp_offset, fp_offset; char *overflow_arg_area, *reg_save_area; }
//
// va_list is an ARRAY of one of those, not the struct itself, because
// "vfprintf(out, fmt, ap)" has to hand glibc the ADDRESS of the struct so
// that glibc's own va_arg advances the caller's copy rather than a duplicate.
//
// This compiler cannot actually tell the two apart: is_address_valued() is
// true for every aggregate, so a plain struct would yield its address here
// too, and that mutation survives the suite. The array is still what C and
// the ABI specify, and it is what stops this from breaking the moment
// aggregates learn to be passed by value (PLAN 13).
static Type *make_va_list_type(void) {
  Member *gp = calloc(1, sizeof(Member));
  gp->name = "gp_offset"; gp->ty = ty_uint; gp->offset = 0;
  Member *fp = calloc(1, sizeof(Member));
  fp->name = "fp_offset"; fp->ty = ty_uint; fp->offset = 4;
  Member *ov = calloc(1, sizeof(Member));
  ov->name = "overflow_arg_area"; ov->ty = pointer_to(ty_char); ov->offset = 8;
  Member *rs = calloc(1, sizeof(Member));
  rs->name = "reg_save_area"; rs->ty = pointer_to(ty_char); rs->offset = 16;
  gp->next = fp; fp->next = ov; ov->next = rs;

  Type *st = calloc(1, sizeof(Type));
  st->kind = TY_STRUCT; st->size = 24; st->align = 8; st->members = gp;
  return array_of(st, 1);
}

static Obj *parse(Token *t) {
  tk = t;
  globals = NULL;
  enter_scope();
  push_scope("va_list")->type_def = make_va_list_type();
  push_scope("__builtin_va_list")->type_def = make_va_list_type();
  while (tk->kind != TK_EOF) {
    VarAttr attr = {0};
    Type *base = declspec(&attr);

    // "struct foo { ... };" declares a tag and nothing else.
    if (consume(";")) continue;

    if (!attr.is_typedef && is_function_decl()) function(base, &attr);
    else global_variable(base, &attr);
  }
  return globals;
}

// --------------------------------------------------------------- preprocessor
//
// Runs between the tokenizer and the parser, and is placed after the parser in
// this file for one reason: #if has to evaluate a constant expression, and
// borrowing const_expr() above is better than growing a second one that would
// then disagree with it.
//
// Directives are the easy half. The hard half is that expansion must stop:
//
//     #define f(x) f(x)
//     f(1)            must become f(1), not loop
//
// C 6.10.3.4 does this by painting a macro blue inside its own expansion.
// Every token produced by expanding f carries f in its *hide set* and can
// never be expanded by f again. The set is per token rather than global
// because the same identifier can be live in one argument and painted in
// another, so a single "currently expanding" flag gives the wrong answer.

typedef struct Macro Macro;
typedef struct MacroParam MacroParam;
typedef struct MacroArg MacroArg;
typedef struct CondIncl CondIncl;

struct HideSet {
  HideSet *next;
  char *name;
};

struct MacroParam {
  MacroParam *next;
  char *name;
};

struct MacroArg {
  MacroArg *next;
  char *name;
  Token *tok;       // the argument as written, EOF-terminated
  Token *expanded;  // the same after expansion, computed once on demand
};

struct Macro {
  Macro *next;
  char *name;
  bool is_objlike;
  bool is_variadic;
  MacroParam *params;
  char *va_name;    // what the variadic tail is called: __VA_ARGS__, or a name
  Token *body;
  Token *(*builtin)(Token *tok);   // __FILE__ and __LINE__ have no fixed body
};

struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;    // some branch of this #if has already been taken
};

static Macro *macros;
static CondIncl *cond_incl;
static char *include_paths[64];
static int n_include_paths;

static Token *preprocess2(Token *tok);
static Token *copy_token(Token *t);
static void read_macro_definition(Token **rest, Token *tok);

// A NUL-terminated copy of a token's spelling.
static char *tok_text(Token *t) {
  char *s = malloc(t->len + 1);
  memcpy(s, t->loc, t->len);
  s[t->len] = 0;
  return s;
}

static Token *copy_token(Token *t) {
  Token *c = calloc(1, sizeof(Token));
  *c = *t;
  c->next = NULL;
  return c;
}

static Token *new_eof(Token *t) {
  Token *e = copy_token(t);
  e->kind = TK_EOF;
  e->len = 0;
  return e;
}

// ---- hide sets ------------------------------------------------------------

static bool hide_contains(HideSet *hs, char *name) {
  for (; hs; hs = hs->next)
    if (!strcmp(hs->name, name)) return true;
  return false;
}

static HideSet *hide_add(HideSet *hs, char *name) {
  HideSet *h = calloc(1, sizeof(HideSet));
  h->name = name;
  h->next = hs;
  return h;
}

static HideSet *hide_union(HideSet *a, HideSet *b) {
  HideSet head = {0};
  HideSet *cur = &head;
  for (; a; a = a->next) {
    cur->next = calloc(1, sizeof(HideSet));
    cur = cur->next;
    cur->name = a->name;
  }
  cur->next = b;
  return head.next;
}

// Function-like macros intersect the hide sets of the name and the closing
// paren. The tokens between them may have come from different expansions, and
// only what both ends agree is painted may be carried forward.
static HideSet *hide_intersect(HideSet *a, HideSet *b) {
  HideSet head = {0};
  HideSet *cur = &head;
  for (; a; a = a->next)
    if (hide_contains(b, a->name)) {
      cur->next = calloc(1, sizeof(HideSet));
      cur = cur->next;
      cur->name = a->name;
    }
  return head.next;
}

// An expansion belongs to where it was written, not where it was defined.
// Without this, __LINE__ inside a macro body reports the #define's line, and
// an error inside an expansion points into the macro instead of the call.
static void stamp_site(Token *tok, Token *site) {
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    tok->file = site->file;
    tok->line = site->line;
  }
}

static Token *add_hide(Token *tok, HideSet *hs) {
  Token head = {0};
  Token *cur = &head;
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    cur = cur->next = copy_token(tok);
    cur->hide = hide_union(cur->hide, hs);
  }
  cur->next = tok ? copy_token(tok) : NULL;
  return head.next;
}

// ---- line handling --------------------------------------------------------

static bool is_hash(Token *t) { return t->at_bol && tok_is(t, "#"); }

// Everything from tok to the end of its line, EOF-terminated, with *rest left
// on the next line. Directives are line-shaped and the rest of the engine is
// not, so this is the seam between the two.
static Token *copy_line(Token **rest, Token *tok) {
  Token head = {0};
  Token *cur = &head;
  for (; tok->kind != TK_EOF && !tok->at_bol; tok = tok->next)
    cur = cur->next = copy_token(tok);
  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

// Advance to the first token of the next line. Note the precondition: tok
// must be a token the caller has already consumed, because a token that is
// itself at_bol is already "the next line" and this returns it unchanged.
// Passing the directive's own '#' therefore makes no progress at all, which
// is how the conditional skipper below hung.
static Token *skip_line(Token *tok) {
  if (tok->kind == TK_EOF) return tok;
  while (tok->kind != TK_EOF && !tok->at_bol) tok = tok->next;
  return tok;
}

// Splice two streams. a's EOF is dropped; b's is kept.
static Token *append(Token *a, Token *b) {
  if (!a || a->kind == TK_EOF) return b;
  Token head = {0};
  Token *cur = &head;
  for (; a && a->kind != TK_EOF; a = a->next)
    cur = cur->next = copy_token(a);
  cur->next = b;
  return head.next;
}

// Tokenize text this pass built rather than read. The buffer is registered as
// a File so that an error inside a paste still gets a caret, pointing at the
// generated text, which is the only place the bad token actually exists.
static Token *tokenize_string(char *name, char *text) {
  // strdup because new_file splices line continuations in place, and some
  // callers here hand over a string literal -- the predefined macros do.
  text = strdup(text);
  File *sf = tk_file;
  int sl = tk_line;
  bool sb = tk_bol, ss = tk_space;
  Token *t = tokenize(new_file(name, text));
  tk_file = sf; tk_line = sl; tk_bol = sb; tk_space = ss;
  return t;
}

// ---- the macro table ------------------------------------------------------

static Macro *find_macro(Token *t) {
  if (t->kind != TK_IDENT) return NULL;
  for (Macro *m = macros; m; m = m->next)
    if ((int)strlen(m->name) == t->len && !strncmp(m->name, t->loc, t->len))
      return m;
  return NULL;
}

static void undef_macro(char *name) {
  Macro **p = &macros;
  while (*p) {
    if (!strcmp((*p)->name, name)) { *p = (*p)->next; continue; }
    p = &(*p)->next;
  }
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  undef_macro(name);   // a redefinition replaces; C requires it be identical,
                       // which we do not police -- see README
  Macro *m = calloc(1, sizeof(Macro));
  m->name = name;
  m->is_objlike = is_objlike;
  m->body = body;
  m->next = macros;
  macros = m;
  return m;
}

static Macro *add_builtin(char *name, Token *(*fn)(Token *)) {
  Macro *m = add_macro(name, true, NULL);
  m->builtin = fn;
  return m;
}

// ---- # and ## -------------------------------------------------------------

// The spelling of a token sequence, with the single space that has_space
// recorded. C requires exactly this: "a + b" stringizes with its spacing
// normalized to one space, and no space where the source had none.
static char *join_tokens(Token *tok, Token *end) {
  int len = 1;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next)
    len += t->len + 1;

  char *buf = calloc(1, len);
  int pos = 0;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (pos && t->has_space) buf[pos++] = ' ';
    memcpy(buf + pos, t->loc, t->len);
    pos += t->len;
  }
  buf[pos] = 0;
  return buf;
}

// #arg. The result is a string literal token, so embedded quotes and
// backslashes have to be escaped on the way in.
static Token *stringize(Token *hash, Token *arg) {
  char *raw = join_tokens(arg, NULL);

  int n = 0;
  for (char *p = raw; *p; p++) n += (*p == '"' || *p == '\\') ? 2 : 1;
  char *quoted = calloc(1, n + 3);
  int pos = 0;
  quoted[pos++] = '"';
  for (char *p = raw; *p; p++) {
    if (*p == '"' || *p == '\\') quoted[pos++] = '\\';
    quoted[pos++] = *p;
  }
  quoted[pos++] = '"';
  quoted[pos] = 0;

  Token *t = tokenize_string("<stringize>", quoted);
  t = copy_token(t);
  t->at_bol = hash->at_bol;
  t->has_space = hash->has_space;
  return t;
}

// lhs ## rhs. The two spellings are concatenated and retokenized, and the
// result must be exactly one token: "+" ## "+" is ++, but "+" ## "x" is not a
// token at all and is an error rather than two tokens pretending to be one.
static Token *paste(Token *lhs, Token *rhs) {
  char *buf = calloc(1, lhs->len + rhs->len + 1);
  memcpy(buf, lhs->loc, lhs->len);
  memcpy(buf + lhs->len, rhs->loc, rhs->len);
  buf[lhs->len + rhs->len] = 0;

  Token *t = tokenize_string("<paste>", buf);
  if (t->kind == TK_EOF || t->next->kind != TK_EOF)
    error_at(lhs->loc, "pasting \"%s\" and \"%s\" does not give a valid token",
             tok_text(lhs), tok_text(rhs));
  t = copy_token(t);
  t->at_bol = lhs->at_bol;
  t->has_space = lhs->has_space;
  return t;
}

// ---- arguments ------------------------------------------------------------

static MacroArg *find_arg(MacroArg *args, Token *t) {
  if (t->kind != TK_IDENT) return NULL;
  for (MacroArg *a = args; a; a = a->next)
    if ((int)strlen(a->name) == t->len && !strncmp(a->name, t->loc, t->len))
      return a;
  return NULL;
}

// One argument: tokens up to a comma or the closing paren, at depth zero.
// Nested parens and commas inside them belong to the argument, which is why
// f(g(1,2)) is one argument and not two.
static Token *read_one_arg(Token **rest, Token *tok, bool to_paren) {
  Token head = {0};
  Token *cur = &head;
  int depth = 0;

  for (;;) {
    if (tok->kind == TK_EOF) error_at(tok->loc, "unterminated macro argument list");
    if (depth == 0 && tok_is(tok, ")")) break;
    if (depth == 0 && !to_paren && tok_is(tok, ",")) break;

    if (tok_is(tok, "(")) depth++;
    else if (tok_is(tok, ")")) depth--;

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }
  cur->next = new_eof(tok);
  *rest = tok;
  return head.next;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, Macro *m) {
  Token *start = tok;
  tok = tok->next->next;   // skip the macro name and '('

  MacroArg head = {0};
  MacroArg *cur = &head;

  for (MacroParam *p = m->params; p; p = p->next) {
    if (cur != &head) {
      if (!tok_is(tok, ",")) error_at(tok->loc, "expected ',' in arguments to '%s'", m->name);
      tok = tok->next;
    }
    cur = cur->next = calloc(1, sizeof(MacroArg));
    cur->name = p->name;
    cur->tok = read_one_arg(&tok, tok, false);
  }

  if (m->is_variadic) {
    Token *rest_tok;
    if (tok_is(tok, ")")) {
      // No variadic arguments at all. The parameter still exists and expands
      // to nothing, which is what makes the GNU ", ## __VA_ARGS__" comma
      // deletion below necessary.
      rest_tok = new_eof(tok);
    } else {
      if (m->params) {
        if (!tok_is(tok, ",")) error_at(tok->loc, "expected ',' before the variadic arguments to '%s'", m->name);
        tok = tok->next;
      }
      rest_tok = read_one_arg(&tok, tok, true);
    }
    cur = cur->next = calloc(1, sizeof(MacroArg));
    cur->name = m->va_name;
    cur->tok = rest_tok;
  } else if (m->params == NULL) {
    // f() with no parameters: consume nothing, but still require the paren.
    if (!tok_is(tok, ")")) error_at(tok->loc, "'%s' takes no arguments", m->name);
  }

  if (!tok_is(tok, ")"))
    error_at(tok->loc, "too many arguments to '%s'", m->name);
  (void)start;
  *rest = tok->next;
  return head.next;
}

// An argument is expanded before substitution, except where it is the operand
// of # or ##. Done once and cached: an argument used twice must not be
// expanded twice, or a macro with a side-effecting expansion changes meaning
// with how often its parameter appears.
static Token *expand_arg(MacroArg *arg) {
  if (!arg->expanded) arg->expanded = preprocess2(arg->tok);
  return arg->expanded;
}

// ---- substitution ---------------------------------------------------------

static Token *subst(Token *body, MacroArg *args) {
  Token head = {0};
  Token *cur = &head;

  while (body && body->kind != TK_EOF) {
    // #param
    if (tok_is(body, "#") && body->next && find_arg(args, body->next)) {
      cur = cur->next = stringize(body, find_arg(args, body->next)->tok);
      body = body->next->next;
      continue;
    }

    // GNU: ", ## __VA_ARGS__" deletes the comma when the tail is empty.
    // glibc's headers are built on this, so a preprocessor that refuses it
    // cannot read /usr/include at all.
    if (tok_is(body, ",") && body->next && tok_is(body->next, "##") &&
        body->next->next && find_arg(args, body->next->next)) {
      MacroArg *arg = find_arg(args, body->next->next);
      if (arg->tok->kind == TK_EOF) {
        body = body->next->next->next;   // drop ',' '##' and the empty arg
        continue;
      }
      cur = cur->next = copy_token(body);        // keep the comma
      body = body->next->next;                   // step over '##'
      for (Token *t = arg->tok; t && t->kind != TK_EOF; t = t->next)
        cur = cur->next = copy_token(t);
      body = body->next;
      continue;
    }

    // lhs ## rhs
    if (tok_is(body, "##")) {
      if (cur == &head) error_at(body->loc, "'##' cannot appear at the start of a macro body");
      if (!body->next || body->next->kind == TK_EOF)
        error_at(body->loc, "'##' cannot appear at the end of a macro body");

      Token *rhs = body->next;
      MacroArg *arg = find_arg(args, rhs);
      if (arg) {
        // Unexpanded, per the standard: the operand of ## is the argument as
        // written.
        if (arg->tok->kind != TK_EOF) {
          *cur = *paste(cur, arg->tok);
          for (Token *t = arg->tok->next; t && t->kind != TK_EOF; t = t->next)
            cur = cur->next = copy_token(t);
        }
      } else {
        *cur = *paste(cur, rhs);
      }
      body = rhs->next;
      continue;
    }

    MacroArg *arg = find_arg(args, body);
    if (arg) {
      // param ## something: the argument is used unexpanded.
      if (body->next && tok_is(body->next, "##")) {
        if (arg->tok->kind == TK_EOF) {
          // An empty argument before ## makes the paste vanish rather than
          // paste against nothing.
          body = body->next->next;
          MacroArg *rhs_arg = find_arg(args, body);
          if (rhs_arg) {
            for (Token *t = rhs_arg->tok; t && t->kind != TK_EOF; t = t->next)
              cur = cur->next = copy_token(t);
          } else if (body && body->kind != TK_EOF) {
            cur = cur->next = copy_token(body);
          }
          if (body && body->kind != TK_EOF) body = body->next;
          continue;
        }
        for (Token *t = arg->tok; t && t->kind != TK_EOF; t = t->next)
          cur = cur->next = copy_token(t);
        body = body->next;
        continue;
      }

      Token *e = expand_arg(arg);
      bool first = true;
      for (Token *t = e; t && t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(t);
        // The spacing of the use site wins over the spacing inside the
        // argument, so "f (x)" and "f(x)" stringize the same.
        if (first) { cur->has_space = body->has_space; cur->at_bol = body->at_bol; first = false; }
      }
      body = body->next;
      continue;
    }

    cur = cur->next = copy_token(body);
    body = body->next;
  }

  cur->next = body ? copy_token(body) : NULL;
  return head.next;
}

// ---- expansion ------------------------------------------------------------

// Expand one macro at tok, if there is one. Returns false when tok is not a
// macro use, which is the common case and must stay cheap.
static bool expand_macro(Token **rest, Token *tok) {
  if (hide_contains(tok->hide, tok_text(tok))) return false;

  Macro *m = find_macro(tok);
  if (!m) return false;

  if (m->builtin) {
    *rest = append(m->builtin(tok), tok->next);
    return true;
  }

  if (m->is_objlike) {
    HideSet *hs = hide_add(tok->hide, m->name);
    Token *body = add_hide(subst(m->body, NULL), hs);
    stamp_site(body, tok);
    if (body) {
      body->at_bol = tok->at_bol;
      body->has_space = tok->has_space;
    }
    *rest = append(body, tok->next);
    return true;
  }

  // A function-like macro name not followed by '(' is just an identifier.
  // This is why "#define getc(f) ..." does not break a function pointer
  // called getc.
  if (!tok->next || !tok_is(tok->next, "(")) return false;

  Token *rparen_probe = tok;
  MacroArg *args = read_macro_args(&rparen_probe, tok, m);
  // read_macro_args leaves rparen_probe after ')', so back up one for the
  // hide set intersection the standard asks for.
  Token *rparen = tok;
  for (Token *t = tok; t != rparen_probe; t = t->next) rparen = t;

  HideSet *hs = hide_intersect(tok->hide, rparen->hide);
  hs = hide_add(hs, m->name);

  Token *body = add_hide(subst(m->body, args), hs);
  stamp_site(body, tok);
  if (body) {
    body->at_bol = tok->at_bol;
    body->has_space = tok->has_space;
  }
  *rest = append(body, rparen_probe);
  return true;
}

// ---- #if ------------------------------------------------------------------

static Token *read_const_expr(Token **rest, Token *tok) {
  tok = copy_line(rest, tok);

  Token head = {0};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(X)" and "defined X" are resolved before expansion, because
    // after expansion X is gone.
    if (tok->kind == TK_IDENT && tok_is(tok, "defined")) {
      Token *start = tok;
      tok = tok->next;
      bool paren = tok_is(tok, "(");
      if (paren) tok = tok->next;
      if (tok->kind != TK_IDENT)
        error_at(start->loc, "'defined' expects an identifier");
      Macro *m = find_macro(tok);
      tok = tok->next;
      if (paren) {
        if (!tok_is(tok, ")")) error_at(start->loc, "expected ')' after 'defined('");
        tok = tok->next;
      }
      cur = cur->next = copy_token(start);
      cur->kind = TK_NUM;
      cur->val = m ? 1 : 0;
      cur->len = 1;
      cur->loc = m ? "1" : "0";
      continue;
    }
    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }
  cur->next = copy_token(tok);
  return head.next;
}

static long eval_const_line(Token **rest, Token *tok) {
  Token *start = tok;
  Token *expr = preprocess2(read_const_expr(rest, tok->next));

  if (expr->kind == TK_EOF)
    error_at(start->loc, "#%s with no expression", tok_text(start->next ? start : start));

  // Any identifier that survived expansion is 0, including 'true', 'false'
  // and anything else undefined. This is C 6.10.1p4, and it is also why the
  // parser below never sees a name it would try to look up.
  for (Token *t = expr; t && t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      t->kind = TK_NUM;
      t->val = 0;
      t->loc = "0";
      t->len = 1;
    }
  }

  // Borrow the real expression parser. Saving and restoring the cursor keeps
  // this reentrant with respect to the parse that has not started yet.
  Token *save = tk;
  tk = expr;
  long v = const_expr();
  if (tk->kind != TK_EOF) error_at(tk->loc, "extra tokens after the #if expression");
  tk = save;
  return v;
}

// ---- conditionals ---------------------------------------------------------

static CondIncl *push_cond(Token *tok, bool included) {
  CondIncl *ci = calloc(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = tok;
  ci->included = included;
  cond_incl = ci;
  return ci;
}

// Skip to the matching #else/#elif/#endif without expanding anything. Nested
// conditionals are counted so that a #endif belonging to an inner #if does not
// end the outer one -- and crucially nothing in here is evaluated, so a
// disabled branch may contain syntax this compiler cannot parse.
static Token *skip_cond_incl(Token *tok) {
  int depth = 0;
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && tok->next->kind != TK_EOF) {
      Token *d = tok->next;
      if (tok_is(d, "if") || tok_is(d, "ifdef") || tok_is(d, "ifndef")) {
        depth++;
      } else if (tok_is(d, "endif")) {
        if (depth == 0) return tok;
        depth--;
      } else if (depth == 0 && (tok_is(d, "else") || tok_is(d, "elif"))) {
        return tok;
      }
    }
    // tok->next, not tok: tok starts its line in every iteration here, and
    // skip_line would hand it straight back.
    tok = skip_line(tok->next);
  }
  return tok;
}

// ---- #include -------------------------------------------------------------

static char *join_path(char *dir, char *file) {
  char *p = malloc(strlen(dir) + strlen(file) + 2);
  sprintf(p, "%s/%s", dir, file);
  return p;
}

static bool file_exists(char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  fclose(f);
  return true;
}

// Which include path the result came from, so #include_next can resume after
// it. -1 means "not from the search path at all".
static int found_in_path;

static char *search_include(char *name, bool is_angle, Token *tok) {
  found_in_path = -1;
  if (name[0] == '/') return file_exists(name) ? name : NULL;

  // A quoted include looks next to the including file first. Using the
  // process's cwd instead is a classic way to find the wrong header when the
  // compiler is run from another directory.
  if (!is_angle && tok->file) {
    char *dir = strdup(tok->file->name);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    char *p = join_path(dir, name);
    if (file_exists(p)) return p;
  }
  for (int i = 0; i < n_include_paths; i++) {
    char *p = join_path(include_paths[i], name);
    if (file_exists(p)) { found_in_path = i; return p; }
  }
  return NULL;
}

// #include_next finds the NEXT header of this name after the one currently
// being read. It is how gcc's own limits.h reaches glibc's: two files with
// the same name in different directories, each deliberately pulling in the
// other. Searching from the start would find this file again and recurse.
static char *search_include_next(char *name, Token *tok) {
  int start = (tok->file ? tok->file->path_index : -1) + 1;
  for (int i = start; i < n_include_paths; i++) {
    char *p = join_path(include_paths[i], name);
    if (file_exists(p)) { found_in_path = i; return p; }
  }
  return NULL;
}

// The filename in an #include is not an ordinary token sequence: <stdio.h>
// tokenizes as '<' 'stdio' '.' 'h' '>', so it is read back from the source
// text between the angle brackets rather than from the tokens.
static char *read_include_filename(Token **rest, Token *tok, bool *is_angle) {
  if (tok->kind == TK_STR) {
    // Not read_char-decoded: a backslash in an include path is literal.
    *is_angle = false;
    *rest = skip_line(tok);
    char *s = calloc(1, tok->len - 1);
    memcpy(s, tok->loc + 1, tok->len - 2);
    return s;
  }

  if (tok_is(tok, "<")) {
    Token *start = tok;
    for (; !tok_is(tok, ">"); tok = tok->next)
      if (tok->at_bol || tok->kind == TK_EOF)
        error_at(start->loc, "unterminated #include <...>");
    *is_angle = true;
    *rest = skip_line(tok);
    int len = tok->loc - (start->loc + 1);
    char *s = calloc(1, len + 1);
    memcpy(s, start->loc + 1, len);
    return s;
  }

  error_at(tok->loc, "#include expects \"FILENAME\" or <FILENAME>");
  return NULL;
}

// ---- builtins -------------------------------------------------------------

// __FILE__ and __LINE__ depend on where they are used, so they are functions
// of the use site rather than a stored body.
static Token *builtin_file(Token *tok) {
  char *name = tok->file ? tok->file->name : "<unknown>";
  char *q = malloc(strlen(name) + 3);
  sprintf(q, "\"%s\"", name);
  Token *t = copy_token(tokenize_string("<__FILE__>", q));
  t->has_space = tok->has_space;
  t->at_bol = tok->at_bol;
  t->next = NULL;
  return t;
}

static Token *builtin_line(Token *tok) {
  char buf[24];
  sprintf(buf, "%d", tok->line);
  Token *t = copy_token(tokenize_string("<__LINE__>", strdup(buf)));
  t->has_space = tok->has_space;
  t->at_bol = tok->at_bol;
  t->next = NULL;
  return t;
}

// Predefine a macro from source text, so a built-in definition goes through
// exactly the same parser a #define does.
static void define_text(char *text) {
  Token *t = tokenize_string("<built-in>", text);
  Token *rest;
  read_macro_definition(&rest, t);
}

// Where <...> is looked for when -I says nothing. The gcc-internal directory
// is first because it owns stddef.h, stdarg.h and friends, which /usr/include
// expects to be supplied by the compiler rather than by libc.
static void init_include_paths(void) {
  static char *defaults[] = {
    "/usr/local/include",
    "/usr/lib/gcc/x86_64-redhat-linux/15/include",
    "/usr/lib/gcc/x86_64-linux-gnu/14/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include",
    NULL,
  };
  for (int i = 0; defaults[i]; i++)
    if (n_include_paths < (int)(sizeof(include_paths) / sizeof(*include_paths)))
      include_paths[n_include_paths++] = defaults[i];
}

static void init_macros(void) {
  add_builtin("__FILE__", builtin_file);
  add_builtin("__LINE__", builtin_line);

  // <stdarg.h> is three macros over compiler builtins and a typedef, and the
  // typedef is already in the parser's file scope. Providing them here means
  // a variadic function needs no header at all, which matters because the
  // real one is GNU C from its first line.
  define_text("va_start(ap, last) __builtin_va_start(ap, last)");
  define_text("va_arg(ap, ty) __builtin_va_arg(ap, ty)");
  define_text("va_end(ap) ((void)0)");
  // The real <stdarg.h> defines va_end in terms of this, and a
  // translation unit that includes it gets that spelling instead.
  define_text("__builtin_va_end(ap) ((void)0)");
  define_text("va_copy(d, s) ((void)((d)[0] = (s)[0]))");

  // gcc's own stdint-gcc.h asks the compiler what its integer types are
  // called, rather than hardcoding them, so these have to exist before
  // <stdint.h> means anything. The values are the LP64 answers.
  define_text("__CHAR_BIT__ 8");
  define_text("__SIZEOF_INT__ 4");
  define_text("__SIZEOF_LONG__ 8");
  define_text("__SIZEOF_POINTER__ 8");
  define_text("__SIZEOF_SHORT__ 2");
  define_text("__SIZE_TYPE__ unsigned long");
  define_text("__PTRDIFF_TYPE__ long");
  define_text("__WCHAR_TYPE__ int");
  define_text("__INTPTR_TYPE__ long");
  define_text("__UINTPTR_TYPE__ unsigned long");
  define_text("__INT8_TYPE__ signed char");
  define_text("__INT16_TYPE__ short");
  define_text("__INT32_TYPE__ int");
  define_text("__INT64_TYPE__ long");
  define_text("__UINT8_TYPE__ unsigned char");
  define_text("__UINT16_TYPE__ unsigned short");
  define_text("__UINT32_TYPE__ unsigned int");
  define_text("__UINT64_TYPE__ unsigned long");
  define_text("__INT_LEAST8_TYPE__ signed char");
  define_text("__INT_LEAST16_TYPE__ short");
  define_text("__INT_LEAST32_TYPE__ int");
  define_text("__INT_LEAST64_TYPE__ long");
  define_text("__UINT_LEAST8_TYPE__ unsigned char");
  define_text("__UINT_LEAST16_TYPE__ unsigned short");
  define_text("__UINT_LEAST32_TYPE__ unsigned int");
  define_text("__UINT_LEAST64_TYPE__ unsigned long");
  define_text("__INT_FAST8_TYPE__ signed char");
  define_text("__INT_FAST16_TYPE__ long");
  define_text("__INT_FAST32_TYPE__ long");
  define_text("__INT_FAST64_TYPE__ long");
  define_text("__UINT_FAST8_TYPE__ unsigned char");
  define_text("__UINT_FAST16_TYPE__ unsigned long");
  define_text("__UINT_FAST32_TYPE__ unsigned long");
  define_text("__UINT_FAST64_TYPE__ unsigned long");
  define_text("__INTMAX_TYPE__ long");
  define_text("__UINTMAX_TYPE__ unsigned long");

  define_text("__STDC__ 1");
  define_text("__x86_64__ 1");
  define_text("__linux__ 1");
}

// ---- #define --------------------------------------------------------------

static void read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT) error_at(tok->loc, "#define expects a macro name");
  char *name = tok_text(tok);
  tok = tok->next;

  // The paren must be glued to the name. "#define A (x)" is an object-like
  // macro whose body is "(x)"; "#define A(x)" is function-like. The only
  // thing that distinguishes them is the absence of a space.
  if (!tok->has_space && tok_is(tok, "(")) {
    tok = tok->next;
    MacroParam head = {0};
    MacroParam *cur = &head;
    bool variadic = false;
    char *va_name = "__VA_ARGS__";

    while (!tok_is(tok, ")")) {
      if (cur != &head) {
        if (!tok_is(tok, ",")) error_at(tok->loc, "expected ',' in the parameter list");
        tok = tok->next;
      }
      if (tok_is(tok, "...")) {
        variadic = true;
        tok = tok->next;
        break;
      }
      if (tok->kind != TK_IDENT) error_at(tok->loc, "expected a parameter name");
      // GNU named variadic: #define E(fmt, args...)
      if (tok->next && tok_is(tok->next, "...")) {
        variadic = true;
        va_name = tok_text(tok);
        tok = tok->next->next;
        break;
      }
      cur = cur->next = calloc(1, sizeof(MacroParam));
      cur->name = tok_text(tok);
      tok = tok->next;
    }
    if (!tok_is(tok, ")")) error_at(tok->loc, "expected ')' to close the parameter list");
    tok = tok->next;

    Macro *m = add_macro(name, false, copy_line(rest, tok));
    m->params = head.next;
    m->is_variadic = variadic;
    m->va_name = va_name;
    return;
  }

  add_macro(name, true, copy_line(rest, tok));
}

// ---- the main loop --------------------------------------------------------

static Token *preprocess2(Token *tok) {
  Token head = {0};
  Token *cur = &head;

  while (tok && tok->kind != TK_EOF) {
    if (expand_macro(&tok, tok)) continue;

    if (!is_hash(tok)) {
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *start = tok;
    tok = tok->next;

    if (tok->at_bol || tok->kind == TK_EOF) continue;   // a bare '#' is a null directive

    if (tok_is(tok, "include") || tok_is(tok, "include_next")) {
      bool next = tok_is(tok, "include_next");
      bool is_angle;
      char *name = read_include_filename(&tok, tok->next, &is_angle);
      char *path = next ? search_include_next(name, start)
                        : search_include(name, is_angle, start);
      if (!path) error_at(start->loc, "cannot find include file '%s'", name);
      File *f = new_file(path, read_file(path));
      f->path_index = found_in_path;
      Token *inc = tokenize(f);
      tok = append(inc, tok);
      continue;
    }

    if (tok_is(tok, "define")) { read_macro_definition(&tok, tok->next); continue; }

    if (tok_is(tok, "undef")) {
      tok = tok->next;
      if (tok->kind != TK_IDENT) error_at(tok->loc, "#undef expects a macro name");
      undef_macro(tok_text(tok));
      tok = skip_line(tok);
      continue;
    }

    if (tok_is(tok, "if")) {
      long v = eval_const_line(&tok, tok);
      push_cond(start, v != 0);
      if (!v) tok = skip_cond_incl(tok);
      continue;
    }

    if (tok_is(tok, "ifdef") || tok_is(tok, "ifndef")) {
      bool want = tok_is(tok, "ifdef");
      tok = tok->next;
      if (tok->kind != TK_IDENT) error_at(tok->loc, "#ifdef expects a macro name");
      bool defined = find_macro(tok) != NULL;
      tok = skip_line(tok);
      push_cond(start, defined == want);
      if (defined != want) tok = skip_cond_incl(tok);
      continue;
    }

    if (tok_is(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_at(start->loc, "#elif without a matching #if");
      cond_incl->ctx = IN_ELIF;
      if (cond_incl->included) {
        tok = skip_cond_incl(skip_line(tok));
      } else {
        long v = eval_const_line(&tok, tok);
        if (v) cond_incl->included = true;
        else tok = skip_cond_incl(tok);
      }
      continue;
    }

    if (tok_is(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE)
        error_at(start->loc, "#else without a matching #if");
      cond_incl->ctx = IN_ELSE;
      tok = skip_line(tok->next);
      if (cond_incl->included) tok = skip_cond_incl(tok);
      continue;
    }

    if (tok_is(tok, "endif")) {
      if (!cond_incl) error_at(start->loc, "#endif without a matching #if");
      cond_incl = cond_incl->next;
      tok = skip_line(tok->next);
      continue;
    }

    if (tok_is(tok, "error")) {
      char *msg = join_tokens(tok->next, NULL);
      error_at(start->loc, "#error %s", msg);
    }

    if (tok_is(tok, "warning")) {
      Token *line = copy_line(&tok, tok->next);
      fprintf(stderr, "%s:%d: warning: %s\n",
              start->file ? start->file->name : "?", start->line, join_tokens(line, NULL));
      continue;
    }

    // #pragma and #line change nothing this compiler can observe. Skipping
    // them beats rejecting them, and both appear in real headers.
    if (tok_is(tok, "pragma") || tok_is(tok, "line")) { tok = skip_line(tok); continue; }

    error_at(tok->loc, "unknown directive #%s", tok_text(tok));
  }

  cur->next = tok;
  return head.next;
}

// Translation phase 6: adjacent string literals become one. This is not a
// preprocessor job in the standard's numbering, but it is the first point at
// which both halves are visible, and the parser would otherwise see two
// expressions side by side.
static void join_adjacent_strings(Token *tok) {
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    if (tok->kind != TK_STR || !tok->next || tok->next->kind != TK_STR) continue;

    int len = tok->str_len - 1;
    Token *t = tok->next;
    for (; t->kind == TK_STR; t = t->next) len += t->str_len - 1;

    char *buf = calloc(1, len + 1);
    int pos = 0;
    memcpy(buf, tok->str, tok->str_len - 1);
    pos += tok->str_len - 1;
    for (Token *u = tok->next; u->kind == TK_STR; u = u->next) {
      memcpy(buf + pos, u->str, u->str_len - 1);
      pos += u->str_len - 1;
    }
    tok->str = buf;
    tok->str_len = len + 1;
    tok->next = t;
  }
}

static void add_include_path(char *path) {
  if (n_include_paths == (int)(sizeof(include_paths) / sizeof(*include_paths)))
    error("too many -I paths");
  include_paths[n_include_paths++] = path;
}

// -DNAME, -DNAME=VALUE. Routed through the real #define parser rather than a
// second one, so a command-line macro behaves exactly like a source-level one,
// function-like forms included.
static void define_from_cli(char *arg) {
  char *eq = strchr(arg, '=');
  char *text;
  if (eq) {
    text = malloc(strlen(arg) + 2);
    memcpy(text, arg, eq - arg);
    text[eq - arg] = ' ';
    strcpy(text + (eq - arg) + 1, eq + 1);
  } else {
    text = malloc(strlen(arg) + 3);
    sprintf(text, "%s 1", arg);
  }
  Token *t = tokenize_string("<command line>", text);
  Token *rest;
  read_macro_definition(&rest, t);
}

// Would printing b straight after a let them lex as something else? The
// output of -E has to read back as the same token stream, and expansion
// routinely puts two tokens next to each other that were never adjacent in
// the source: "2" and "+" become "2+", which a pp-number happily swallows as
// the start of an exponent.
//
// This is gcc's rule rather than a clever one. A pp-number is treated as
// hazardous before any of digit, letter, '.', '+' or '-' without looking at
// what the number actually is, because "1e" and "2" are the same kind of
// token and only one of them is dangerous. Being conservative costs a space.
static bool needs_space(Token *a, Token *b) {
  if (!a || b->kind == TK_EOF) return false;
  char bc = b->loc[0];

  if (a->kind == TK_NUM)
    return isdigit((unsigned char)bc) || is_ident1(bc) || bc == '.' || bc == '+' || bc == '-';

  if (a->kind == TK_IDENT || a->kind == TK_KEYWORD)
    return is_ident1(bc) || isdigit((unsigned char)bc);

  if (a->kind == TK_PUNCT && b->kind == TK_PUNCT) {
    // '/' then '/' or '*' would open a comment, which no punctuator table
    // will tell you about.
    if (a->loc[0] == '/' && (bc == '/' || bc == '*')) return true;

    char glued[8];
    int n = a->len + b->len;
    if (n >= (int)sizeof(glued)) return false;
    memcpy(glued, a->loc, a->len);
    memcpy(glued + a->len, b->loc, b->len);
    glued[n] = 0;
    // If the longest punctuator at the join is longer than a was, the two
    // have merged into a third thing: '+' '+' is '++'.
    return read_punct(glued) > a->len;
  }
  return false;
}

// -E. Printed from the tokens rather than the text, because after expansion
// there is no text: at_bol and has_space are the only record of the original
// shape, which is exactly what they were kept for.
static void print_tokens(Token *tok, char *path) {
  FILE *o = (!path || !strcmp(path, "-")) ? stdout : fopen(path, "w");
  if (!o) error("cannot write %s", path);

  Token *prev = NULL;
  for (; tok && tok->kind != TK_EOF; tok = tok->next) {
    if (prev && tok->at_bol) fprintf(o, "\n");
    else if (prev && (tok->has_space || needs_space(prev, tok))) fprintf(o, " ");
    fprintf(o, "%.*s", tok->len, tok->loc);
    prev = tok;
  }
  fprintf(o, "\n");
  if (o != stdout) fclose(o);
}

// Translation phases 1 to 4. Deliberately stops there: string concatenation
// and keyword recognition are phases 6 and 7, and -E must show the result of
// phase 4 exactly. Folding them in here is what made `S(a) S(b)` print one
// string -- the two literals were correctly concatenated, one phase too early
// for anyone to see them.
static Token *preprocess(Token *tok) {
  init_include_paths();
  init_macros();
  tok = preprocess2(tok);
  if (cond_incl)
    error_at(cond_incl->tok->loc, "unterminated conditional directive");
  return tok;
}

// ---------------------------------------------------------------- code output

static FILE *out;
static int depth;          // values currently pushed on the stack
static int label_id = 1;
static Obj *current_fn;

static void emit(char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(out, "  ");
  vfprintf(out, fmt, ap);
  fprintf(out, "\n");
  va_end(ap);
}

static void push(void) { emit("push %%rax"); depth++; }
static void pop(char *reg) { emit("pop %s", reg); depth--; }

// Argument registers in SysV order, at each width. A table beats doing string
// surgery on "%rdi" to get "%dil" at every call site.
// How many integer registers a by-value aggregate occupies, or 0 for the
// MEMORY class, which this compiler does not pass.
//
// The full SysV classification algorithm walks every field and merges a
// class per eightbyte, because a double lands in an SSE register and an int
// does not. This compiler has no floating-point registers at all, so every
// field that can hold a value is INTEGER and the algorithm collapses to a
// size check -- with one hole that has to be closed by hand: float and
// double are *declarable* here (real headers need them), so a struct
// containing one would be classified SSE by anyone else and must be refused
// rather than quietly passed in the wrong register file.
static bool has_fp_member(Type *ty) {
  if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE) return true;
  if (ty->kind == TY_ARRAY) return has_fp_member(ty->base);
  if (is_aggregate(ty))
    for (Member *m = ty->members; m; m = m->next)
      if (has_fp_member(m->ty)) return true;
  return false;
}

static int aggregate_regs(Type *ty) {
  if (ty->size > 16) return 0;            // MEMORY: passed on the stack
  if (has_fp_member(ty)) return 0;        // SSE class, which we cannot do
  return (ty->size + 7) / 8;              // one or two INTEGER eightbytes
}

static char *ARGREG64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8",  "%r9"};
static char *ARGREG32[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static char *ARGREG16[] = {"%di",  "%si",  "%dx",  "%cx",  "%r8w", "%r9w"};
static char *ARGREG8[]  = {"%dil", "%sil", "%dl",  "%cl",  "%r8b", "%r9b"};

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

// Put the address of an lvalue into rax.
static void gen_addr(Node *n) {
  switch (n->kind) {
  case ND_VAR:
    if (n->var->is_local) emit("lea %d(%%rbp), %%rax", n->var->offset);
    else                  emit("lea %s(%%rip), %%rax", n->var->name);
    return;
  case ND_DEREF:
    gen_expr(n->lhs);
    return;
  case ND_MEMBER:
    gen_addr(n->lhs);
    if (n->member->offset) emit("add $%d, %%rax", n->member->offset);
    return;
  case ND_COMMA:
    gen_expr(n->lhs);
    gen_addr(n->rhs);
    return;
  default:
    // An aggregate expression always evaluates to its own address here, so
    // for one of those "take the address" and "evaluate" are the same
    // instruction sequence. That is what lets a struct-returning call be
    // used where a struct is wanted -- id(id(a)) -- even though a call is
    // not an lvalue in C and cannot be assigned to.
    if (n->ty && is_aggregate(n->ty)) { gen_expr(n); return; }
    error_at(n->tok->loc, "not an lvalue");
  }
}

// rax holds an address; replace it with the value at that address.
static void load(Type *ty) {
  // An array or an aggregate has no loadable value: the address *is* the value.
  if (is_address_valued(ty)) return;

  // The line between declarable and operable. A float may be named in a
  // declaration all day; the moment one has to be read into a register
  // there is no register class for it here, and inventing one silently
  // would miscompile rather than refuse.
  if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE)
    error("floating point is not implemented: a float or double can be "
          "declared, which is what real headers need, but not used");

  // Narrow loads must widen with the right sign, or a char holding 0xFF reads
  // as 255 where it should read as -1 (or the reverse for unsigned char).
  if (ty->size == 1)      emit(ty->is_unsigned ? "movzbl (%%rax), %%eax" : "movsbl (%%rax), %%eax");
  else if (ty->size == 2) emit(ty->is_unsigned ? "movzwl (%%rax), %%eax" : "movswl (%%rax), %%eax");
  else if (ty->size == 4) emit("movl (%%rax), %%eax");
  else                    emit("mov (%%rax), %%rax");
}

// Stack holds the destination address; rax holds the value.
static void store(Type *ty) {
  pop("%rdi");

  if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE)
    error("floating point is not implemented: a float or double can be "
          "declared, which is what real headers need, but not used");

  if (is_aggregate(ty)) {
    // rax holds the source address, rdi the destination. Copy byte by byte.
    // shortcut: unrolled, so a 4KB struct emits 8K instructions. Switch to a
    // rep movsb or a loop once anything in t/ has a struct big enough to care.
    for (int i = 0; i < ty->size; i++) {
      emit("mov %d(%%rax), %%r8b", i);
      emit("mov %%r8b, %d(%%rdi)", i);
    }
    emit("mov %%rdi, %%rax");       // the assignment's value is the destination
    return;
  }

  if (ty->size == 1)      emit("mov %%al, (%%rdi)");
  else if (ty->size == 2) emit("mov %%ax, (%%rdi)");
  else if (ty->size == 4) emit("mov %%eax, (%%rdi)");
  else                    emit("mov %%rax, (%%rdi)");
}

static void cmp_zero(Type *ty) {
  if (is_integer(ty) && ty->size <= 4) emit("cmp $0, %%eax");
  else                                 emit("cmp $0, %%rax");
}

// Convert the value in rax from one type to another. The rules that matter:
// narrowing truncates, widening sign- or zero-extends depending on the SOURCE's
// signedness, and anything to _Bool is a comparison against zero.
static void gen_cast(Type *from, Type *to) {
  if (to->kind == TY_VOID) return;

  if (to->kind == TY_BOOL) {
    cmp_zero(from);
    emit("setne %%al");
    emit("movzb %%al, %%rax");
    return;
  }

  if (is_pointer(to) || to->size == 8) {
    // Widening to 64 bits: the source's signedness decides the extension.
    if (is_integer(from) && from->size < 8)
      emit(from->is_unsigned ? "mov %%eax, %%eax" : "movsxd %%eax, %%rax");
    return;
  }

  if (to->size == 1)      emit(to->is_unsigned ? "movzbl %%al, %%eax" : "movsbl %%al, %%eax");
  else if (to->size == 2) emit(to->is_unsigned ? "movzwl %%ax, %%eax" : "movswl %%ax, %%eax");
  else if (to->size == 4) emit("mov %%eax, %%eax");     // truncate to 32 bits
}

// Does this binary operation compare and divide as unsigned?
//
// C 6.3.1.8's usual arithmetic conversions decide it from BOTH operands, and
// reading only the left one is wrong in a way the parser makes easy to miss:
// "x > 5" is built as ND_LT with the operands swapped, so the literal ends up
// on the left. An unsigned x therefore compared signed, while "x < 5" on the
// same variable compared unsigned.
//
// The rule, for the integer types this compiler has:
//   - promote anything narrower than int to signed int, since an int holds
//     every value an unsigned char or unsigned short can have
//   - the wider rank wins outright, which is why "long < unsigned int" stays
//     signed on LP64: a long represents every unsigned int
//   - at equal rank, unsigned wins
static bool op_unsigned(Type *a, Type *b) {
  if (is_pointer(a) || is_pointer(b)) return true;   // addresses never go negative

  int asz = a->size < 4 ? 4 : a->size;
  int bsz = b->size < 4 ? 4 : b->size;
  bool au = a->size < 4 ? false : a->is_unsigned;
  bool bu = b->size < 4 ? false : b->is_unsigned;

  if (asz != bsz) return asz > bsz ? au : bu;
  return au || bu;
}

static void gen_expr(Node *n) {
  switch (n->kind) {
  case ND_NUM:
    emit("mov $%ld, %%rax", n->val);
    return;
  case ND_NEG:
    gen_expr(n->lhs);
    emit("neg %%rax");
    return;
  case ND_BITNOT:
    gen_expr(n->lhs);
    emit("not %%rax");
    return;
  case ND_VAR:
    gen_addr(n);
    load(n->ty);
    return;
  case ND_ADDR:
    gen_addr(n->lhs);
    return;
  case ND_DEREF:
    gen_expr(n->lhs);
    load(n->ty);
    return;
  case ND_MEMBER:
    // Reading a member is addressing it and then loading. Without this case it
    // falls through to the binary-operator path and dereferences a NULL rhs.
    gen_addr(n);
    load(n->ty);
    if (n->member->is_bitfield) {
      // The load brought in the whole storage unit. Put the field in the top
      // of the register and arithmetic-shift it back down: that is what
      // sign-extends a signed field of an unusual width, and it costs the
      // same as masking would.
      Member *m = n->member;
      int total = m->ty->size * 8;
      emit("shl $%d, %%rax", 64 - m->bit_width - m->bit_offset);
      emit(m->ty->is_unsigned ? "shr $%d, %%rax" : "sar $%d, %%rax", 64 - m->bit_width);
      (void)total;
    }
    return;
  case ND_CAST:
    gen_expr(n->lhs);
    gen_cast(n->lhs->ty, n->ty);
    return;
  case ND_ASSIGN:
    gen_addr(n->lhs);
    push();
    gen_expr(n->rhs);
    if (n->lhs->kind == ND_MEMBER && n->lhs->member->is_bitfield) {
      // Read, modify, write. A bitfield shares its storage unit with its
      // neighbours, so a plain store would overwrite them; and the value of
      // the assignment is the field as it reads back, truncated to its
      // width, not the value that was offered.
      Member *m = n->lhs->member;
      // Unsigned, because the mask is shifted below and shifting a negative
      // value left is undefined. This compiler's own sanitizer build is what
      // said so, on the first 64-bit-wide field anyone wrote.
      unsigned long mask =
          m->bit_width == 64 ? ~0UL : ((1UL << m->bit_width) - 1);

      emit("mov %%rax, %%r8");                     // keep the assigned value
      pop("%rdi");                                 // rdi = address of the unit
      // Through a register, not an immediate: AND takes at most a 32-bit
      // immediate, and a 32-bit-wide field's mask does not fit in one. GAS
      // quietly promotes "mov $imm64" to movabs and has no such escape for
      // "and", so the only sign of the problem is the assembler refusing it.
      emit("mov $%ld, %%r9", (long)mask);
      emit("and %%r9, %%rax");                     // the bits that belong to us
      emit("shl $%d, %%rax", m->bit_offset);       // moved into place

      // Load the unit, clear our window, OR the new bits in.
      if (m->ty->size == 1)      emit("movzbl (%%rdi), %%ecx");
      else if (m->ty->size == 2) emit("movzwl (%%rdi), %%ecx");
      else if (m->ty->size == 4) emit("movl (%%rdi), %%ecx");
      else                       emit("mov (%%rdi), %%rcx");
      emit("mov $%ld, %%r9", (long)~(mask << m->bit_offset));
      emit("and %%r9, %%rcx");
      emit("or %%rcx, %%rax");

      if (m->ty->size == 1)      emit("mov %%al, (%%rdi)");
      else if (m->ty->size == 2) emit("mov %%ax, (%%rdi)");
      else if (m->ty->size == 4) emit("mov %%eax, (%%rdi)");
      else                       emit("mov %%rax, (%%rdi)");

      // The result of the assignment is what the field now holds, which for
      // a signed field that overflowed its width is not what was assigned.
      emit("mov %%r8, %%rax");
      emit("shl $%d, %%rax", 64 - m->bit_width);
      emit(m->ty->is_unsigned ? "shr $%d, %%rax" : "sar $%d, %%rax", 64 - m->bit_width);
      return;
    }
    store(n->ty);
    return;
  case ND_COMMA:
    gen_expr(n->lhs);
    gen_expr(n->rhs);
    return;
  case ND_NOT:
    gen_expr(n->lhs);
    cmp_zero(n->lhs->ty);
    emit("sete %%al");
    emit("movzb %%al, %%rax");
    return;
  case ND_COND: {
    int c = label_id++;
    gen_expr(n->cond);
    cmp_zero(n->cond->ty);
    emit("je .L.else.%d", c);
    gen_expr(n->then);
    emit("jmp .L.end.%d", c);
    fprintf(out, ".L.else.%d:\n", c);
    gen_expr(n->els);
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_AND: {
    int c = label_id++;
    gen_expr(n->lhs);
    cmp_zero(n->lhs->ty);
    emit("je .L.false.%d", c);
    gen_expr(n->rhs);
    cmp_zero(n->rhs->ty);
    emit("je .L.false.%d", c);
    emit("mov $1, %%rax");
    emit("jmp .L.end.%d", c);
    fprintf(out, ".L.false.%d:\n", c);
    emit("mov $0, %%rax");
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_OR: {
    int c = label_id++;
    gen_expr(n->lhs);
    cmp_zero(n->lhs->ty);
    emit("jne .L.true.%d", c);
    gen_expr(n->rhs);
    cmp_zero(n->rhs->ty);
    emit("jne .L.true.%d", c);
    emit("mov $0, %%rax");
    emit("jmp .L.end.%d", c);
    fprintf(out, ".L.true.%d:\n", c);
    emit("mov $1, %%rax");
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_VA_START: {
    // ap is an array of one struct, so evaluating it yields its address.
    gen_expr(n->lhs);
    int named = 0;
    for (Obj *pv = current_fn->params; pv; pv = pv->next) named++;
    // gp_offset starts past the slots the named parameters occupy, so the
    // first va_arg returns the first argument the caller actually added.
    emit("movl $%d, (%%rax)", named * 8);
    emit("movl $48, 4(%%rax)");          // no float registers are ever used
    // Arguments seven and beyond were pushed by the caller and live above
    // the return address: rbp+16 is the first of them.
    emit("lea 16(%%rbp), %%rdi");
    emit("mov %%rdi, 8(%%rax)");
    emit("lea %d(%%rbp), %%rdi", current_fn->va_area);
    emit("mov %%rdi, 16(%%rax)");
    return;
  }
  case ND_VA_ARG: {
    gen_expr(n->lhs);                     // rax = &va_list
    int lbl = label_id++;
    // if (gp_offset < 48) take it from the register save area, else from
    // the overflow area. The branch is the whole of va_arg.
    emit("movl (%%rax), %%ecx");
    emit("cmp $48, %%ecx");
    emit("jae .L.va.over.%d", lbl);
    emit("mov 16(%%rax), %%rdx");         // reg_save_area
    emit("add %%rcx, %%rdx");
    emit("add $8, %%ecx");
    emit("movl %%ecx, (%%rax)");
    emit("jmp .L.va.done.%d", lbl);
    emit(".L.va.over.%d:", lbl);
    emit("mov 8(%%rax), %%rdx");          // overflow_arg_area
    emit("lea 8(%%rdx), %%rcx");
    emit("mov %%rcx, 8(%%rax)");
    emit(".L.va.done.%d:", lbl);
    // Every integer argument occupies a full eight-byte slot, so a narrower
    // type is the low bytes of one rather than a tighter load.
    if (n->ty->size == 1)      emit(n->ty->is_unsigned ? "movzbl (%%rdx), %%eax" : "movsbl (%%rdx), %%eax");
    else if (n->ty->size == 2) emit(n->ty->is_unsigned ? "movzwl (%%rdx), %%eax" : "movswl (%%rdx), %%eax");
    else if (n->ty->size == 4) emit("movl (%%rdx), %%eax");
    else                       emit("mov (%%rdx), %%rax");
    return;
  }
  case ND_FUNCALL: {
    // One argument is no longer one register: a small aggregate occupies
    // one or two. Eightbytes are pushed left to right in the order the
    // registers want them, so popping the whole run in reverse still lands
    // each one where it belongs.
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) {
      if (is_aggregate(a->ty)) {
        int regs = aggregate_regs(a->ty);
        if (!regs)
          error_at(a->tok->loc, "passing a %d-byte struct or union by value is not "
                                "supported (the MEMORY class needs stack arguments; "
                                "see PLAN.md)", a->ty->size);
        gen_addr(a);
        emit("mov %%rax, %%r11");          // the object; %r11 survives push()
        for (int i = 0; i < regs; i++) {
          int rest = a->ty->size - i * 8;
          if (rest >= 8) {
            emit("mov %d(%%r11), %%rax", i * 8);
          } else {
            // Assemble the tail byte by byte rather than reading a full
            // eightbyte past the end of the object. The bytes beyond it
            // belong to something else, and under ASan that is a report.
            emit("xor %%eax, %%eax");
            for (int k = rest - 1; k >= 0; k--) {
              emit("shl $8, %%rax");
              emit("movzbl %d(%%r11), %%ecx", i * 8 + k);
              emit("or %%rcx, %%rax");
            }
          }
          push();
          nargs++;
        }
        continue;
      }
      gen_expr(a);
      push();
      nargs++;
    }
    if (nargs > 6) error_at(n->tok->loc, "more than 6 argument registers is not supported");

    // The callee address is computed before the argument registers are
    // loaded, and parked on the stack: evaluating it could call something
    // else, and anything it calls would clobber them.
    if (n->callee) {
      gen_expr(n->callee);
      push();
    }
    // The callee was pushed after the arguments, so it is on top and has to
    // come off first. Popping the arguments first puts the callee in an
    // argument register and the last argument in %r10, and the call jumps
    // to whatever the argument happened to be.
    if (n->callee) pop("%r10");
    for (int i = nargs - 1; i >= 0; i--) pop(ARGREG64[i]);

    // The ABI wants %rsp 16-byte aligned at the call. depth counts our pushes;
    // an odd count means we are 8 off.
    emit("mov $0, %%rax");        // no vector registers used, for varargs
    if (depth % 2 != 0) emit("sub $8, %%rsp");
    if (n->callee) emit("call *%%r10");
    else           emit("call %s", n->funcname);
    if (depth % 2 != 0) emit("add $8, %%rsp");

    // A returned aggregate arrives in rax:rdx, but every aggregate here is
    // addressed rather than held, so it is written into a hidden local and
    // the call's value becomes that local's address.
    if (n->ret_buf) {
      int sz = n->ret_buf->ty->size;
      for (int i = 0; i < (sz + 7) / 8; i++) {
        char *src64 = (i == 0) ? "%rax" : "%rdx";
        char *src32 = (i == 0) ? "%eax" : "%edx";
        char *src16 = (i == 0) ? "%ax"  : "%dx";
        char *src8  = (i == 0) ? "%al"  : "%dl";
        int rest = sz - i * 8, off = n->ret_buf->offset + i * 8;
        if (rest >= 8)      emit("mov %s, %d(%%rbp)", src64, off);
        else if (rest == 4) emit("mov %s, %d(%%rbp)", src32, off);
        else if (rest == 2) emit("mov %s, %d(%%rbp)", src16, off);
        else if (rest == 1) emit("mov %s, %d(%%rbp)", src8,  off);
        else {
          emit("mov %s, %%r11", src64);
          int done = 0;
          if (rest >= 4) { emit("mov %%r11d, %d(%%rbp)", off); emit("shr $32, %%r11"); done = 4; }
          if (rest - done >= 2) { emit("mov %%r11w, %d(%%rbp)", off + done); emit("shr $16, %%r11"); done += 2; }
          if (rest - done >= 1) { emit("mov %%r11b, %d(%%rbp)", off + done); }
        }
      }
      emit("lea %d(%%rbp), %%rax", n->ret_buf->offset);
    }
    return;
  }
  default:
    break;
  }

  // Binary operators: evaluate rhs first so lhs lands in rax.
  gen_expr(n->rhs);
  push();
  gen_expr(n->lhs);
  pop("%rdi");

  // Use 32-bit forms when both operands are narrow, so that int arithmetic
  // wraps at 32 bits the way C requires.
  // A shift is not symmetric: the result is as wide as what is being
  // shifted, and the count's own width says nothing. Including it made
  // "1L << 32" a 32-bit shift whenever the count was an int, and x86 masks
  // a 32-bit shift count to 5 bits, so the answer was 1 rather than 2^32.
  bool shift = (n->kind == ND_SHL || n->kind == ND_SHR);
  bool wide = shift
      ? (n->lhs->ty->size == 8 || is_pointer(n->lhs->ty))
      : (n->lhs->ty->size == 8 || n->rhs->ty->size == 8 || is_pointer(n->lhs->ty));
  char *ax = wide ? "%rax" : "%eax";
  char *di = wide ? "%rdi" : "%edi";
  // Signedness decides div vs idiv and setb vs setl.
  bool uns = op_unsigned(n->lhs->ty, n->rhs->ty);

  switch (n->kind) {
  case ND_ADD: emit("add %s, %s", di, ax); return;
  case ND_SUB: emit("sub %s, %s", di, ax); return;
  case ND_MUL: emit("imul %s, %s", di, ax); return;
  case ND_DIV:
  case ND_MOD:
    if (uns) {
      // Unsigned division zeroes the high half rather than sign-extending it.
      if (wide) { emit("mov $0, %%rdx"); emit("div %%rdi"); }
      else      { emit("mov $0, %%edx"); emit("div %%edi"); }
      if (n->kind == ND_MOD) emit("mov %%rdx, %%rax");
    } else if (wide) {
      emit("cqo"); emit("idiv %%rdi");
      if (n->kind == ND_MOD) emit("mov %%rdx, %%rax");
    } else {
      emit("cdq"); emit("idiv %%edi");
      // Sign-extend: idiv writes %edx, which zero-extends into %rdx, so a
      // negative remainder would read as a large positive 64-bit value.
      if (n->kind == ND_MOD) emit("movslq %%edx, %%rax");
    }
    return;
  case ND_BITAND: emit("and %s, %s", di, ax); return;
  case ND_BITOR:  emit("or %s, %s", di, ax); return;
  case ND_BITXOR: emit("xor %s, %s", di, ax); return;
  case ND_SHL:    emit("mov %%rdi, %%rcx"); emit("shl %%cl, %s", ax); return;
  case ND_SHR:
    emit("mov %%rdi, %%rcx");
    emit(uns ? "shr %%cl, %s" : "sar %%cl, %s", ax);
    return;
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
    emit("cmp %s, %s", di, ax);
    if (n->kind == ND_EQ)      emit("sete %%al");
    else if (n->kind == ND_NE) emit("setne %%al");
    else if (n->kind == ND_LT) emit(uns ? "setb %%al"  : "setl %%al");
    else                       emit(uns ? "setbe %%al" : "setle %%al");
    emit("movzb %%al, %%rax");
    return;
  default:
    error_at(n->tok->loc, "invalid expression");
  }
}

static void gen_stmt(Node *n) {
  switch (n->kind) {
  case ND_BLOCK:
    for (Node *b = n->body; b; b = b->next) gen_stmt(b);
    return;
  case ND_EXPR_STMT:
    gen_expr(n->lhs);
    return;
  case ND_RETURN:
    if (n->lhs) {
      gen_expr(n->lhs);
      // An aggregate expression yields its address, so returning one means
      // loading the eightbytes the ABI wants in rax and rdx. Done here and
      // not at the epilogue because only this node knows what is returned.
      if (is_aggregate(n->lhs->ty)) {
        int sz = n->lhs->ty->size;
        emit("mov %%rax, %%r11");
        for (int i = 0; i < (sz + 7) / 8; i++) {
          char *dst = (i == 0) ? "%rax" : "%rdx";
          int rest = sz - i * 8;
          if (rest >= 8) {
            emit("mov %d(%%r11), %s", i * 8, dst);
          } else {
            emit("xor %s, %s", (i == 0) ? "%eax" : "%edx", (i == 0) ? "%eax" : "%edx");
            for (int k = rest - 1; k >= 0; k--) {
              emit("shl $8, %s", dst);
              emit("movzbl %d(%%r11), %%ecx", i * 8 + k);
              emit("or %%rcx, %s", dst);
            }
          }
        }
      }
    }
    emit("jmp .L.return.%s", current_fn->name);
    return;
  case ND_IF: {
    int c = label_id++;
    gen_expr(n->cond);
    cmp_zero(n->cond->ty);
    emit("je .L.else.%d", c);
    gen_stmt(n->then);
    emit("jmp .L.end.%d", c);
    fprintf(out, ".L.else.%d:\n", c);
    if (n->els) gen_stmt(n->els);
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_FOR: {
    int c = label_id++;
    char brk[32], cont[32];
    snprintf(brk, sizeof(brk), ".L.brk.%d", c);
    snprintf(cont, sizeof(cont), ".L.cont.%d", c);
    char *sb = brk_label, *sc = cont_label;
    brk_label = brk; cont_label = cont;

    if (n->init) gen_stmt(n->init);
    fprintf(out, ".L.begin.%d:\n", c);
    if (n->cond) {
      gen_expr(n->cond);
      cmp_zero(n->cond->ty);
      emit("je %s", brk);
    }
    gen_stmt(n->then);
    fprintf(out, "%s:\n", cont);
    if (n->inc) gen_expr(n->inc);
    emit("jmp .L.begin.%d", c);
    fprintf(out, "%s:\n", brk);

    brk_label = sb; cont_label = sc;
    return;
  }
  case ND_DO: {
    int c = label_id++;
    char brk[32], cont[32];
    snprintf(brk, sizeof(brk), ".L.brk.%d", c);
    snprintf(cont, sizeof(cont), ".L.cont.%d", c);
    char *sb = brk_label, *sc = cont_label;
    brk_label = brk; cont_label = cont;

    fprintf(out, ".L.begin.%d:\n", c);
    gen_stmt(n->then);
    fprintf(out, "%s:\n", cont);
    gen_expr(n->cond);
    cmp_zero(n->cond->ty);
    emit("jne .L.begin.%d", c);
    fprintf(out, "%s:\n", brk);

    brk_label = sb; cont_label = sc;
    return;
  }
  case ND_SWITCH: {
    int c = label_id++;
    char brk[32];
    snprintf(brk, sizeof(brk), ".L.brk.%d", c);
    char *sb = brk_label;
    brk_label = brk;

    gen_expr(n->cond);
    char *reg = (n->cond->ty->size == 8) ? "%rax" : "%eax";

    // A linear chain of compares. shortcut: a jump table would be faster for a
    // dense switch; nothing in t/ has enough cases for it to show up.
    for (Node *cs = n->case_next; cs; cs = cs->case_next) {
      cs->case_lbl = calloc(1, 40);
      snprintf(cs->case_lbl, 40, ".L.case.%d.%d", c, label_id++);
      emit("cmp $%ld, %s", cs->case_val, reg);
      emit("je %s", cs->case_lbl);
    }

    if (n->default_case) {
      n->default_case->case_lbl = calloc(1, 40);
      snprintf(n->default_case->case_lbl, 40, ".L.default.%d", c);
      emit("jmp %s", n->default_case->case_lbl);
    } else {
      emit("jmp %s", brk);
    }

    gen_stmt(n->then);
    fprintf(out, "%s:\n", brk);
    brk_label = sb;
    return;
  }
  case ND_CASE:
    // Fallthrough is the default and comes for free: the label is emitted inline
    // and control simply continues into whatever follows.
    fprintf(out, "%s:\n", n->case_lbl);
    gen_stmt(n->lhs);
    return;
  case ND_GOTO:
    emit("jmp %s", n->unique_label);
    return;
  case ND_LABEL:
    fprintf(out, "%s:\n", n->unique_label);
    gen_stmt(n->lhs);
    return;
  case ND_BREAK:
    if (!brk_label) error_at(n->tok->loc, "break outside of a loop or switch");
    emit("jmp %s", brk_label);
    return;
  case ND_CONTINUE:
    if (!cont_label) error_at(n->tok->loc, "continue outside of a loop");
    emit("jmp %s", cont_label);
    return;
  default:
    error_at(n->tok->loc, "invalid statement");
  }
}

static void emit_data(Obj *globals_) {
  for (Obj *v = globals_; v; v = v->next) {
    if (v->is_function) continue;
    // extern is a promise that someone else defines it. Emitting .zero here
    // would make this file a second definition of the same symbol.
    if (v->is_extern) continue;

    if (v->init_data) {
      // String literals are read-only; everything else may be written to.
      fprintf(out, v->is_anon_str ? "  .section .rodata\n" : "  .data\n");
      if (!v->is_static && !v->is_anon_str) fprintf(out, "  .globl %s\n", v->name);
      fprintf(out, "%s:\n", v->name);

      Relocation *r = v->rel;
      int i = 0;
      while (i < v->init_len) {
        if (r && r->offset == i) {
          // Eight bytes the linker fills in, not eight bytes we know.
          fprintf(out, "  .quad %s%+ld\n", r->label, r->addend);
          r = r->next;
          i += 8;
          continue;
        }
        fprintf(out, "  .byte %d\n", (unsigned char)v->init_data[i]);
        i++;
      }
      continue;
    }
    fprintf(out, "  .bss\n");
    if (!v->is_static) fprintf(out, "  .globl %s\n", v->name);
    fprintf(out, "%s:\n", v->name);
    fprintf(out, "  .zero %d\n", v->ty->size);
  }
}

static void emit_text(Obj *globals_) {
  for (Obj *fn = globals_; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition) continue;
    current_fn = fn;

    fprintf(out, "  .text\n");
    if (!fn->is_static) fprintf(out, "  .globl %s\n", fn->name);
    fprintf(out, "  .type %s, @function\n", fn->name);
    fprintf(out, "%s:\n", fn->name);

    emit("push %%rbp");
    emit("mov %%rsp, %%rbp");
    if (fn->stack_size) emit("sub $%d, %%rsp", fn->stack_size);

    // All six, unconditionally, before the named ones are spilled to their
    // own slots. The named arguments are therefore stored twice, which is
    // what the ABI asks for: the save area has to be a contiguous image of
    // the registers so that va_arg can index into it by offset.
    if (fn->is_variadic)
      for (int i = 0; i < 6; i++)
        emit("mov %s, %d(%%rbp)", ARGREG64[i], fn->va_area + i * 8);

    // fn->params is in declaration order, so the registers are consumed in
    // that order too -- but a small aggregate consumes one or two of them,
    // so the index advances by what each parameter actually takes rather
    // than by one.
    int k = 0;
    for (Obj *p = fn->params; p; p = p->next) {
      if (is_aggregate(p->ty)) {
        int regs = aggregate_regs(p->ty);
        for (int i = 0; i < regs && k < 6; i++, k++) {
          int rest = p->ty->size - i * 8;
          int off = p->offset + i * 8;
          // The tail is written at its real width. Storing a full eightbyte
          // for a 12-byte struct would trample whatever follows it in the
          // frame, which is a different local.
          if (rest >= 8)      emit("mov %s, %d(%%rbp)", ARGREG64[k], off);
          else if (rest == 4) emit("mov %s, %d(%%rbp)", ARGREG32[k], off);
          else if (rest == 2) emit("mov %s, %d(%%rbp)", ARGREG16[k], off);
          else if (rest == 1) emit("mov %s, %d(%%rbp)", ARGREG8[k],  off);
          else {
            // 3, 5, 6 or 7 bytes: store the widest pieces that fit.
            emit("mov %s, %%r11", ARGREG64[k]);
            int done = 0;
            if (rest >= 4) { emit("mov %%r11d, %d(%%rbp)", off); emit("shr $32, %%r11"); done = 4; }
            if (rest - done >= 2) { emit("mov %%r11w, %d(%%rbp)", off + done); emit("shr $16, %%r11"); done += 2; }
            if (rest - done >= 1) { emit("mov %%r11b, %d(%%rbp)", off + done); }
          }
        }
        continue;
      }
      if (k >= 6) break;
      if (p->ty->size == 1)      emit("mov %s, %d(%%rbp)", ARGREG8[k],  p->offset);
      else if (p->ty->size == 2) emit("mov %s, %d(%%rbp)", ARGREG16[k], p->offset);
      else if (p->ty->size == 4) emit("mov %s, %d(%%rbp)", ARGREG32[k], p->offset);
      else                       emit("mov %s, %d(%%rbp)", ARGREG64[k], p->offset);
      k++;
    }

    gen_stmt(fn->body);
    if (depth != 0) error("internal: stack depth %d after %s", depth, fn->name);

    // Falling off the end of a non-void function returns 0, like gcc does in
    // practice. main especially relies on this.
    emit("mov $0, %%rax");
    fprintf(out, ".L.return.%s:\n", fn->name);
    emit("mov %%rbp, %%rsp");
    emit("pop %%rbp");
    emit("ret");
  }
}

// ------------------------------------------------------------------ the driver

static char *read_file(char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) error("cannot open %s", path);
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(n + 2);
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  buf[got] = 0;
  if (got > 0 && buf[got - 1] != '\n') { buf[got] = '\n'; buf[got + 1] = 0; }
  return buf;
}

static void run(char **argv) {
  pid_t pid = fork();
  if (pid < 0) error("fork failed");
  if (pid == 0) {
    execvp(argv[0], argv);
    fprintf(stderr, "cc: cannot exec %s\n", argv[0]);
    _exit(127);
  }
  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) exit(1);
}

static void usage(int status) {
  fprintf(stderr,
      "usage: cc [-E] [-S] [-o OUT] [-Idir] [-Dname[=val]] [-Uname] FILE.c\n"
      "  -E  preprocess only, and print the token stream\n"
      "  -S  compile to assembly, do not assemble or link\n");
  exit(status);
}

int main(int argc, char **argv) {
  char *input = NULL;
  char *output = NULL;
  bool asm_only = false;
  bool preproc_only = false;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) usage(0);
    if (!strcmp(argv[i], "-S")) { asm_only = true; continue; }
    if (!strcmp(argv[i], "-E")) { preproc_only = true; continue; }
    if (!strcmp(argv[i], "-I")) {
      if (++i == argc) usage(1);
      add_include_path(argv[i]);
      continue;
    }
    if (!strncmp(argv[i], "-I", 2)) { add_include_path(argv[i] + 2); continue; }
    if (!strcmp(argv[i], "-D")) {
      if (++i == argc) usage(1);
      define_from_cli(argv[i]);
      continue;
    }
    if (!strncmp(argv[i], "-D", 2)) { define_from_cli(argv[i] + 2); continue; }
    if (!strcmp(argv[i], "-U")) {
      if (++i == argc) usage(1);
      undef_macro(argv[i]);
      continue;
    }
    if (!strncmp(argv[i], "-U", 2)) { undef_macro(argv[i] + 2); continue; }
    if (!strcmp(argv[i], "-o")) {
      if (++i == argc) usage(1);
      output = argv[i];
      continue;
    }
    if (!strncmp(argv[i], "-o", 2)) { output = argv[i] + 2; continue; }
    if (argv[i][0] == '-' && argv[i][1]) error("unknown option %s", argv[i]);
    input = argv[i];
  }
  if (!input) usage(1);

  File *f = new_file(input, read_file(input));

  Token *t = preprocess(tokenize(f));

  if (preproc_only) {
    print_tokens(t, output);
    return 0;
  }

  join_adjacent_strings(t);   // phase 6
  convert_keywords(t);        // phase 7
  Obj *prog = parse(t);

  char *asm_path;
  if (asm_only) {
    asm_path = output ? output : "-";
  } else {
    asm_path = malloc(strlen(input) + 16);
    sprintf(asm_path, "%s.cc.s", input);
  }

  out = !strcmp(asm_path, "-") ? stdout : fopen(asm_path, "w");
  if (!out) error("cannot write %s", asm_path);

  fprintf(out, "  .file \"%s\"\n", input);
  emit_data(prog);
  emit_text(prog);
  fprintf(out, "  .section .note.GNU-stack,\"\",@progbits\n");

  if (out != stdout) fclose(out);
  if (asm_only) return 0;

  // shortcut: gcc does as+ld for us. Writing an assembler is ../ld's job.
  char *link[] = {"gcc", "-static", asm_path, "-o", output ? output : "a.out", NULL};
  run(link);
  unlink(asm_path);
  return 0;
}
