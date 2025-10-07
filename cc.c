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

static char *src_name;
static char *src_text;

static void verror_at(char *loc, char *fmt, va_list ap) {
  // Find the line containing loc.
  char *line = loc;
  while (src_text < line && line[-1] != '\n') line--;
  char *end = loc;
  while (*end && *end != '\n') end++;

  int lineno = 1;
  for (char *p = src_text; p < line; p++)
    if (*p == '\n') lineno++;

  int indent = fprintf(stderr, "%s:%d: ", src_name, lineno);
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
struct Token {
  TokenKind kind;
  Token *next;
  long val;      // TK_NUM
  char *loc;     // position in src_text
  int len;
  char *str;     // TK_STR contents (NUL-terminated)
  int str_len;   // including the terminating NUL
};

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *t = calloc(1, sizeof(Token));
  t->kind = kind;
  t->loc = start;
  t->len = end - start;
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
  "int", "char", "long", "short", "void", "sizeof", NULL,
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
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "->",
  "+", "-", "*", "/", "%", "(", ")", "{", "}", "[", "]",
  "<", ">", "=", ";", ",", "&", "|", "^", "~", "!", "?", ":", ".",
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

static Token *tokenize(void) {
  char *p = src_text;
  Token head = {0};
  Token *cur = &head;

  while (*p) {
    if (isspace((unsigned char)*p)) { p++; continue; }

    if (starts_with(p, "//")) {
      while (*p && *p != '\n') p++;
      continue;
    }
    if (starts_with(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q) error_at(p, "unterminated block comment");
      p = q + 2;
      continue;
    }

    if (isdigit((unsigned char)*p)) {
      char *start = p;
      long v = strtol(p, &p, 0);
      while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') p++;  // ignore suffixes
      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = v;
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
      if (is_keyword(cur)) cur->kind = TK_KEYWORD;
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

  cur->next = new_token(TK_EOF, p, p);
  return head.next;
}

// ----------------------------------------------------------------- type system

typedef enum { TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_PTR, TY_ARRAY, TY_FUNC } TypeKind;

typedef struct Type Type;
struct Type {
  TypeKind kind;
  int size;
  Type *base;        // TY_PTR / TY_ARRAY element type
  int array_len;
  Type *ret;         // TY_FUNC
};

static Type *ty_void  = &(Type){TY_VOID,  1};
static Type *ty_char  = &(Type){TY_CHAR,  1};
static Type *ty_short = &(Type){TY_SHORT, 2};
static Type *ty_int   = &(Type){TY_INT,   4};
static Type *ty_long  = &(Type){TY_LONG,  8};

static Type *pointer_to(Type *base) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_PTR; t->size = 8; t->base = base;
  return t;
}

static Type *array_of(Type *base, int len) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_ARRAY; t->size = base->size * len; t->base = base; t->array_len = len;
  return t;
}

static Type *func_returning(Type *ret) {
  Type *t = calloc(1, sizeof(Type));
  t->kind = TY_FUNC; t->size = 8; t->ret = ret;
  return t;
}

static bool is_integer(Type *t) {
  return t->kind == TY_CHAR || t->kind == TY_SHORT || t->kind == TY_INT || t->kind == TY_LONG;
}
static bool is_pointer(Type *t) { return t->kind == TY_PTR || t->kind == TY_ARRAY; }

// ------------------------------------------------------------------------ AST

typedef enum {
  ND_NUM, ND_VAR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_NEG,
  ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_ADDR, ND_DEREF,
  ND_AND, ND_OR, ND_NOT, ND_BITAND, ND_BITOR, ND_BITXOR, ND_BITNOT,
  ND_SHL, ND_SHR, ND_COND, ND_COMMA, ND_CAST,
  ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_BLOCK, ND_FUNCALL, ND_EXPR_STMT,
  ND_BREAK, ND_CONTINUE,
} NodeKind;

typedef struct Node Node;
typedef struct Obj Obj;

struct Obj {
  Obj *next;
  char *name;
  Type *ty;
  bool is_local;
  bool is_function;
  bool is_definition;
  // local
  int offset;
  // global data
  char *init_data;   // non-NULL for string literals
  int init_len;
  // function
  Obj *params;
  Obj *locals;
  Node *body;
  int stack_size;
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
  char *funcname;
  Node *args;

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
static Node *new_var_node(Obj *var, Token *tok) {
  Node *n = new_node(ND_VAR, tok); n->var = var; n->ty = var->ty; return n;
}
static Node *new_cast(Node *expr, Type *ty) {
  Node *n = new_node(ND_CAST, expr->tok); n->lhs = expr; n->ty = ty; return n;
}

// Propagate types bottom-up. Called once per node after it is built.
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
    n->ty = n->lhs->ty;
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
    n->ty = ty_long;   // shortcut: no prototypes yet, so assume long. See PLAN.md.
    return;
  default:
    return;
  }
}

// ----------------------------------------------------------------- the parser

static Token *tk;              // cursor
static Obj *locals;            // current function's locals
static Obj *globals;
static char *brk_label;        // innermost loop labels, or NULL
static char *cont_label;

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
  return v;
}

static Obj *find_var(Token *t) {
  for (Obj *v = locals; v; v = v->next)
    if ((int)strlen(v->name) == t->len && !memcmp(t->loc, v->name, t->len)) return v;
  for (Obj *v = globals; v; v = v->next)
    if ((int)strlen(v->name) == t->len && !memcmp(t->loc, v->name, t->len)) return v;
  return NULL;
}

static int anon_id = 0;
static Obj *new_string_literal(Token *t) {
  char name[32];
  snprintf(name, sizeof(name), ".L.str.%d", anon_id++);
  Obj *v = new_var(strdup(name), array_of(ty_char, t->str_len), false);
  v->init_data = t->str;
  v->init_len = t->str_len;
  return v;
}

static Node *expr(void);
static Node *assign(void);
static Node *stmt(void);
static Node *compound_stmt(void);
static Type *declspec(void);
static Type *declarator(Type *ty, char **name);

static bool is_typename(void) {
  return peek("int") || peek("char") || peek("long") || peek("short") || peek("void");
}

// declspec = ("int" | "char" | "short" | "long" | "void")+
// We accept the common multi-word spellings ("long long", "unsigned" is not
// supported) and collapse them to one of our five base types.
static Type *declspec(void) {
  if (consume("void")) return ty_void;
  if (consume("char")) return ty_char;
  if (consume("short")) { consume("int"); return ty_short; }
  if (consume("long")) { consume("long"); consume("int"); return ty_long; }
  if (consume("int")) return ty_int;
  error_at(tk->loc, "expected a type name");
  return NULL;
}

// Trailing array dimensions: "[3][4]" applied right-to-left.
static Type *type_suffix(Type *ty) {
  if (consume("[")) {
    if (tk->kind != TK_NUM) error_at(tk->loc, "expected an array size");
    int len = tk->val;
    tk = tk->next;
    expect("]");
    return array_of(type_suffix(ty), len);
  }
  return ty;
}

static Type *declarator(Type *ty, char **name) {
  while (consume("*")) ty = pointer_to(ty);
  *name = get_ident();
  return type_suffix(ty);
}

// declaration = declspec declarator ("=" expr)? ("," declarator ("=" expr)?)* ";"
static Node *declaration(void) {
  Type *base = declspec();
  Node head = {0};
  Node *cur = &head;
  bool first = true;

  while (!consume(";")) {
    if (!first) expect(",");
    first = false;
    char *name;
    Type *ty = declarator(base, &name);
    if (ty->kind == TY_VOID) error_at(tk->loc, "variable declared void");
    Obj *var = new_var(name, ty, true);

    if (consume("=")) {
      Node *lhs = new_var_node(var, tk);
      Node *rhs = assign();
      Node *a = new_binary(ND_ASSIGN, lhs, rhs, tk);
      add_type(a);
      cur = cur->next = new_unary(ND_EXPR_STMT, a, tk);
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
  Node *scaled = new_binary(ND_MUL, rhs, new_num(sz, tok), tok);
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
    Node *scaled = new_binary(ND_MUL, rhs, new_num(sz, tok), tok);
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

static Node *funcall(char *name, Token *tok) {
  Node head = {0};
  Node *cur = &head;
  while (!consume(")")) {
    if (cur != &head) expect(",");
    cur = cur->next = assign();
    add_type(cur);
  }
  Node *n = new_node(ND_FUNCALL, tok);
  n->funcname = name;
  n->args = head.next;
  return n;
}

// primary = "(" expr ")" | "sizeof" unary | ident ("(" args ")")? | num | str
static Node *unary(void);
static Node *postfix(void);

static Node *primary(void) {
  Token *tok = tk;

  if (consume("(")) {
    Node *n = expr();
    expect(")");
    return n;
  }

  if (consume("sizeof")) {
    // sizeof(type) vs sizeof expr. Peek for a typename after '('.
    if (peek("(")) {
      Token *save = tk;
      tk = tk->next;
      if (is_typename()) {
        Type *ty = declspec();
        while (consume("*")) ty = pointer_to(ty);
        expect(")");
        return new_num(ty->size, tok);
      }
      tk = save;
    }
    Node *n = unary();
    add_type(n);
    return new_num(n->ty->size, tok);
  }

  if (tk->kind == TK_NUM) {
    Node *n = new_num(tk->val, tk);
    tk = tk->next;
    return n;
  }

  if (tk->kind == TK_STR) {
    Obj *v = new_string_literal(tk);
    tk = tk->next;
    return new_var_node(v, tok);
  }

  if (tk->kind == TK_IDENT) {
    char *name = get_ident();
    if (consume("(")) return funcall(name, tok);
    Obj *v = find_var(tok);
    if (!v) error_at(tok->loc, "undefined variable '%s'", name);
    return new_var_node(v, tok);
  }

  error_at(tk->loc, "expected an expression");
  return NULL;
}

// postfix = primary ("[" expr "]" | "++" | "--")*
static Node *postfix(void) {
  Node *n = primary();
  for (;;) {
    Token *tok = tk;
    if (consume("[")) {               // a[i] is *(a+i), exactly as C says
      Node *idx = expr();
      expect("]");
      n = new_unary(ND_DEREF, new_add(n, idx, tok), tok);
      add_type(n);
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
  if (consume("+")) return unary();
  if (consume("-")) { Node *n = new_unary(ND_NEG, unary(), tok); add_type(n); return n; }
  if (consume("*")) { Node *n = new_unary(ND_DEREF, unary(), tok); add_type(n); return n; }
  if (consume("&")) { Node *n = new_unary(ND_ADDR, unary(), tok); add_type(n); return n; }
  if (consume("!")) { Node *n = new_unary(ND_NOT, unary(), tok); add_type(n); return n; }
  if (consume("~")) { Node *n = new_unary(ND_BITNOT, unary(), tok); add_type(n); return n; }
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

static Node *mul(void) {
  Node *n = unary();
  for (;;) {
    Token *tok = tk;
    if (consume("*"))      { n = new_binary(ND_MUL, n, unary(), tok); add_type(n); }
    else if (consume("/")) { n = new_binary(ND_DIV, n, unary(), tok); add_type(n); }
    else if (consume("%")) { n = new_binary(ND_MOD, n, unary(), tok); add_type(n); }
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
    char *sb = brk_label, *sc = cont_label;
    brk_label = cont_label = NULL;   // assigned in codegen

    if (!peek(";")) n->init = is_typename() ? declaration() : expr_stmt();
    else { tk = tk->next; }
    if (!peek(";")) { n->cond = expr(); add_type(n->cond); }
    expect(";");
    if (!peek(")")) { n->inc = expr(); add_type(n->inc); }
    expect(")");
    n->then = stmt();

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

  if (consume("break"))    { expect(";"); return new_node(ND_BREAK, tok); }
  if (consume("continue")) { expect(";"); return new_node(ND_CONTINUE, tok); }

  if (consume("{")) return compound_stmt();

  return expr_stmt();
}

static Node *compound_stmt(void) {
  Token *tok = tk;
  Node head = {0};
  Node *cur = &head;
  // shortcut: one flat scope per function, not per block. Shadowing in an
  // inner block resolves to the outer variable. Fix when self-hosting needs it.
  while (!consume("}"))
    cur = cur->next = is_typename() ? declaration() : stmt();
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
static Obj *function(Type *base) {
  char *name;
  Type *ty = base;
  while (consume("*")) ty = pointer_to(ty);
  name = get_ident();
  expect("(");

  locals = NULL;

  // Collect parameter types first; we create the Obj's after so that their
  // stack offsets come out in declaration order.
  Obj phead = {0};
  Obj *pcur = &phead;
  if (peek("void") && tok_is(tk->next, ")")) tk = tk->next;   // f(void)
  bool pfirst = true;
  while (!consume(")")) {
    if (!pfirst) expect(",");
    pfirst = false;
    if (consume("...")) { expect(")"); break; }   // varargs: callee ignores them
    Type *pbase = declspec();
    char *pname;
    Type *pty = declarator(pbase, &pname);
    if (pty->kind == TY_ARRAY) pty = pointer_to(pty->base);   // arrays decay
    Obj *p = calloc(1, sizeof(Obj));
    p->name = pname; p->ty = pty;
    pcur = pcur->next = p;
  }

  Obj *fn = calloc(1, sizeof(Obj));
  fn->name = name;
  fn->ty = func_returning(ty);
  fn->is_function = true;
  fn->next = globals;
  globals = fn;

  create_param_vars(phead.next);
  fn->params = locals;

  if (consume(";")) { fn->is_definition = false; return fn; }   // prototype

  expect("{");
  fn->is_definition = true;
  fn->body = compound_stmt();
  fn->locals = locals;

  int off = 0;
  for (Obj *v = fn->locals; v; v = v->next) {
    off += v->ty->size;
    off = align_to(off, v->ty->size > 8 ? 8 : v->ty->size);
    v->offset = -off;
  }
  fn->stack_size = align_to(off, 16);
  return fn;
}

static void global_variable(Type *base) {
  bool first = true;
  while (!consume(";")) {
    if (!first) expect(",");
    first = false;
    char *name;
    Type *ty = declarator(base, &name);
    new_var(name, ty, false);
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

static Obj *parse(Token *t) {
  tk = t;
  globals = NULL;
  while (tk->kind != TK_EOF) {
    Type *base = declspec();
    if (is_function_decl()) function(base);
    else global_variable(base);
  }
  return globals;
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
  case ND_COMMA:
    gen_expr(n->lhs);
    gen_addr(n->rhs);
    return;
  default:
    error_at(n->tok->loc, "not an lvalue");
  }
}

// rax holds an address; replace it with the value at that address.
static void load(Type *ty) {
  if (ty->kind == TY_ARRAY) return;        // arrays evaluate to their address
  if (ty->size == 1)      emit("movsbl (%%rax), %%eax");
  else if (ty->size == 2) emit("movswl (%%rax), %%eax");
  else if (ty->size == 4) emit("movl (%%rax), %%eax");
  else                    emit("mov (%%rax), %%rax");
}

// Stack holds the destination address; rax holds the value.
static void store(Type *ty) {
  pop("%rdi");
  if (ty->size == 1)      emit("mov %%al, (%%rdi)");
  else if (ty->size == 2) emit("mov %%ax, (%%rdi)");
  else if (ty->size == 4) emit("mov %%eax, (%%rdi)");
  else                    emit("mov %%rax, (%%rdi)");
}

static void cmp_zero(Type *ty) {
  if (is_integer(ty) && ty->size <= 4) emit("cmp $0, %%eax");
  else                                 emit("cmp $0, %%rax");
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
  case ND_CAST:
    gen_expr(n->lhs);
    return;
  case ND_ASSIGN:
    gen_addr(n->lhs);
    push();
    gen_expr(n->rhs);
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
  case ND_FUNCALL: {
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) {
      gen_expr(a);
      push();
      nargs++;
    }
    if (nargs > 6) error_at(n->tok->loc, "more than 6 arguments is not supported");
    for (int i = nargs - 1; i >= 0; i--) pop(ARGREG64[i]);

    // The ABI wants %rsp 16-byte aligned at the call. depth counts our pushes;
    // an odd count means we are 8 off.
    emit("mov $0, %%rax");        // no vector registers used, for varargs
    if (depth % 2 == 0) {
      emit("call %s", n->funcname);
    } else {
      emit("sub $8, %%rsp");
      emit("call %s", n->funcname);
      emit("add $8, %%rsp");
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
  bool wide = (n->lhs->ty->size == 8 || n->rhs->ty->size == 8 || is_pointer(n->lhs->ty));
  char *ax = wide ? "%rax" : "%eax";
  char *di = wide ? "%rdi" : "%edi";

  switch (n->kind) {
  case ND_ADD: emit("add %s, %s", di, ax); return;
  case ND_SUB: emit("sub %s, %s", di, ax); return;
  case ND_MUL: emit("imul %s, %s", di, ax); return;
  case ND_DIV:
  case ND_MOD:
    if (wide) {
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
  case ND_SHR:    emit("mov %%rdi, %%rcx"); emit("sar %%cl, %s", ax); return;
  case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
    emit("cmp %s, %s", di, ax);
    if (n->kind == ND_EQ)      emit("sete %%al");
    else if (n->kind == ND_NE) emit("setne %%al");
    else if (n->kind == ND_LT) emit("setl %%al");
    else                       emit("setle %%al");
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
    gen_expr(n->lhs);
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
  case ND_BREAK:
    if (!brk_label) error_at(n->tok->loc, "break outside of a loop");
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

    if (v->init_data) {
      fprintf(out, "  .section .rodata\n");
      fprintf(out, "%s:\n", v->name);
      for (int i = 0; i < v->init_len; i++)
        fprintf(out, "  .byte %d\n", (unsigned char)v->init_data[i]);
      continue;
    }
    fprintf(out, "  .data\n");
    fprintf(out, "  .globl %s\n", v->name);
    fprintf(out, "%s:\n", v->name);
    fprintf(out, "  .zero %d\n", v->ty->size);
  }
}

static void emit_text(Obj *globals_) {
  for (Obj *fn = globals_; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition) continue;
    current_fn = fn;

    fprintf(out, "  .text\n");
    fprintf(out, "  .globl %s\n", fn->name);
    fprintf(out, "  .type %s, @function\n", fn->name);
    fprintf(out, "%s:\n", fn->name);

    emit("push %%rbp");
    emit("mov %%rsp, %%rbp");
    if (fn->stack_size) emit("sub $%d, %%rsp", fn->stack_size);

    // fn->params is already in declaration order, so param k lives in
    // ARGREG*[k]. Spill each to its stack slot on entry.
    int k = 0;
    for (Obj *p = fn->params; p && k < 6; p = p->next, k++) {
      if (p->ty->size == 1)      emit("mov %s, %d(%%rbp)", ARGREG8[k],  p->offset);
      else if (p->ty->size == 2) emit("mov %s, %d(%%rbp)", ARGREG16[k], p->offset);
      else if (p->ty->size == 4) emit("mov %s, %d(%%rbp)", ARGREG32[k], p->offset);
      else                       emit("mov %s, %d(%%rbp)", ARGREG64[k], p->offset);
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
  fprintf(stderr, "usage: cc [-S] [-o OUT] FILE.c\n");
  exit(status);
}

int main(int argc, char **argv) {
  char *input = NULL;
  char *output = NULL;
  bool asm_only = false;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) usage(0);
    if (!strcmp(argv[i], "-S")) { asm_only = true; continue; }
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

  src_name = input;
  src_text = read_file(input);

  Token *t = tokenize();
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
