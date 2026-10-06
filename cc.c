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
  "int", "char", "long", "short", "void", "sizeof", "typedef",
  "struct", "union", "enum", "switch", "case", "default", "goto",
  "unsigned", "signed", "_Bool", NULL,
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

typedef enum {
  TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_ENUM,
  TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION, TY_FUNC,
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
static bool is_address_valued(Type *t) { return t->kind == TY_ARRAY || is_aggregate(t); }

// ------------------------------------------------------------------------ AST

typedef enum {
  ND_NUM, ND_VAR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_NEG,
  ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_ADDR, ND_DEREF,
  ND_AND, ND_OR, ND_NOT, ND_BITAND, ND_BITOR, ND_BITXOR, ND_BITNOT,
  ND_SHL, ND_SHR, ND_COND, ND_COMMA, ND_CAST, ND_MEMBER,
  ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_BLOCK, ND_FUNCALL, ND_EXPR_STMT,
  ND_BREAK, ND_CONTINUE, ND_SWITCH, ND_CASE, ND_GOTO, ND_LABEL,
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
static Node *new_cast(Node *expr, Type *ty) {
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
    n->ty = ty_long;   // shortcut: no prototypes yet, so assume long. See PLAN.md.
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
typedef struct { bool is_typedef; } VarAttr;

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
  push_scope(name)->var = v;
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
  return v;
}

static Node *expr(void);
static Node *assign(void);
static Node *conditional(void);
static Node *stmt(void);
static Node *compound_stmt(void);
static Type *declspec(VarAttr *attr);
static Type *declarator(Type *ty, char **name);
static Type *struct_union_decl(void);
static Type *enum_decl(void);
static long const_expr(void);

static char *TYPE_KEYWORDS[] = {
  "int", "char", "long", "short", "void", "unsigned", "signed", "_Bool",
  "struct", "union", "enum", "typedef", NULL,
};

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
  // Storage class first, and it may appear before or after the type keywords.
  for (;;) {
    if (peek("typedef")) {
      if (!attr) error_at(tk->loc, "typedef is not allowed here");
      tk = tk->next;
      attr->is_typedef = true;
      continue;
    }
    break;
  }

  // "unsigned" and "signed" may stand alone (meaning int) or qualify a width.
  bool is_unsigned = false, saw_sign = false;
  while (peek("unsigned") || peek("signed")) {
    is_unsigned = peek("unsigned");
    saw_sign = true;
    tk = tk->next;
  }

  Type *ty = NULL;
  if (consume("void")) ty = ty_void;
  else if (consume("_Bool")) ty = ty_bool;
  else if (consume("char")) ty = is_unsigned ? ty_uchar : ty_char;
  else if (consume("short")) { consume("int"); ty = is_unsigned ? ty_ushort : ty_short; }
  else if (consume("long")) { consume("long"); consume("int"); ty = is_unsigned ? ty_ulong : ty_long; }
  else if (consume("int")) ty = is_unsigned ? ty_uint : ty_int;
  else if (saw_sign) ty = is_unsigned ? ty_uint : ty_int;   // bare "unsigned"
  else if (peek("struct") || peek("union")) ty = struct_union_decl();
  else if (peek("enum")) ty = enum_decl();
  else {
    Type *td = find_typedef(tk);
    if (!td) error_at(tk->loc, "expected a type name");
    tk = tk->next;
    ty = td;
  }

  // Trailing storage class: "int typedef x;" is legal, if unloved.
  while (peek("typedef")) {
    if (!attr) error_at(tk->loc, "typedef is not allowed here");
    tk = tk->next;
    attr->is_typedef = true;
  }
  return ty;
}

// Trailing array dimensions: "[3][4]" applied right-to-left.
// Fold an integer constant expression. Needed for enum values and array
// dimensions, both of which must be known at compile time.
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
  if (consume("[")) {
    // A dimension may be any constant expression: [N], [2*N], [sizeof(x)].
    long len = const_expr();
    if (len < 0) error_at(tk->loc, "negative array size");
    expect("]");
    return array_of(type_suffix(ty), (int)len);
  }
  return ty;
}

static Type *declarator(Type *ty, char **name) {
  while (consume("*")) ty = pointer_to(ty);
  *name = get_ident();
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
      char *name;
      Type *mty = declarator(base, &name);
      if (mty->kind == TY_VOID) error_at(tk->loc, "member declared void");
      if (mty->size < 0) error_at(tk->loc, "member has incomplete type");
      Member *m = calloc(1, sizeof(Member));
      m->name = name;
      m->ty = mty;
      cur = cur->next = m;
    }
  }
  ty->members = head.next;

  // Layout. A union overlays its members at offset 0 and takes the widest size;
  // a struct packs them in order with each one aligned, then rounds the total up
  // to the struct's own alignment so arrays of it stay aligned.
  int offset = 0;
  for (Member *m = ty->members; m; m = m->next) {
    if (is_union) {
      m->offset = 0;
      if (ty->size < m->ty->size) ty->size = m->ty->size;
    } else {
      offset = align_to(offset, m->ty->align);
      m->offset = offset;
      offset += m->ty->size;
    }
    if (ty->align < m->ty->align) ty->align = m->ty->align;
  }
  if (!is_union) ty->size = offset;
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

// declaration = declspec declarator ("=" expr)? ("," declarator ("=" expr)?)* ";"
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
    if (ty->size < 0) error_at(tk->loc, "variable has incomplete type");
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
static Node *cast_expr(void);

// abstract-declarator, i.e. a type with no identifier attached.
static Type *typename_(void) {
  Type *ty = declspec(NULL);
  while (consume("*")) ty = pointer_to(ty);
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
static Node *cast_expr(void) {
  if (peek("(")) {
    Token *save = tk;
    tk = tk->next;
    if (is_typename()) {
      Type *ty = typename_();
      expect(")");
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
    Type *pbase = declspec(NULL);
    char *pname;
    Type *pty = declarator(pbase, &pname);
    if (pty->kind == TY_ARRAY) pty = pointer_to(pty->base);   // arrays decay
    Obj *p = calloc(1, sizeof(Obj));
    p->name = pname; p->ty = pty;
    pcur = pcur->next = p;
  }

  for (Obj *p = phead.next; p; p = p->next)
    if (is_aggregate(p->ty))
      error_at(tk->loc, "passing a struct or union by value is not supported yet "
                        "(needs SysV eightbyte classification; see PLAN.md)");
  if (is_aggregate(ty))
    error_at(tk->loc, "returning a struct or union by value is not supported yet "
                      "(needs SysV eightbyte classification; see PLAN.md)");

  Obj *fn = calloc(1, sizeof(Obj));
  fn->name = name;
  fn->ty = func_returning(ty);
  fn->is_function = true;
  fn->next = globals;
  globals = fn;

  // Parameters and body share one scope, so a parameter can be shadowed by a
  // declaration in an inner block but not by one at the top level of the body.
  enter_scope();
  create_param_vars(phead.next);
  fn->params = locals;

  if (consume(";")) {           // prototype
    leave_scope();
    fn->is_definition = false;
    return fn;
  }

  expect("{");
  fn->is_definition = true;
  gotos = labels = NULL;
  fn->body = compound_stmt();
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
    if (ty->size < 0) error_at(tk->loc, "global has incomplete type");
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
    VarAttr attr = {0};
    Type *base = declspec(&attr);

    // "struct foo { ... };" declares a tag and nothing else.
    if (consume(";")) continue;

    if (!attr.is_typedef && is_function_decl()) function(base);
    else global_variable(base, &attr);
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
  case ND_MEMBER:
    gen_addr(n->lhs);
    if (n->member->offset) emit("add $%d, %%rax", n->member->offset);
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
  // An array or an aggregate has no loadable value: the address *is* the value.
  if (is_address_valued(ty)) return;

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
    return;
  case ND_CAST:
    gen_expr(n->lhs);
    gen_cast(n->lhs->ty, n->ty);
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
  // Signedness decides div vs idiv and setb vs setl. Pointers compare unsigned.
  bool uns = n->lhs->ty->is_unsigned || is_pointer(n->lhs->ty);

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
