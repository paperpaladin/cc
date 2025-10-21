#!/bin/sh
# Each case compiles a program, runs it, and checks its exit status.
# Exit status is the oracle because it needs no I/O and no libc.
cd "$(dirname "$0")/.." || exit 1

pass=0
fail=0

check() {
  expected="$1"
  src="$2"
  printf '%s' "$src" > t/tmp.c
  if ! ./cc t/tmp.c -o t/tmp 2> t/tmp.err; then
    fail=$((fail + 1))
    printf 'FAIL (compile) %s\n' "$src"
    sed 's/^/        /' t/tmp.err
    return
  fi
  ./t/tmp
  got=$?
  if [ "$got" = "$expected" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL  %s\n        expected %s, got %s\n' "$src" "$expected" "$got"
  fi
}

# --- literals and arithmetic
check 0  'int main() { return 0; }'
check 42 'int main() { return 42; }'
check 21 'int main() { return 5+20-4; }'
check 41 'int main() { return  12 + 34 - 5 ; }'
check 47 'int main() { return 5+6*7; }'
check 15 'int main() { return 5*(9-6); }'
check 4  'int main() { return (3+5)/2; }'
check 10 'int main() { return -10+20; }'
check 10 'int main() { return - -10; }'
check 2  'int main() { return 17%5; }'
check 6  'int main() { return -17%5+8; }'

# --- comparison
check 1  'int main() { return 1==1; }'
check 0  'int main() { return 1!=1; }'
check 1  'int main() { return 0<1; }'
check 0  'int main() { return 1<1; }'
check 1  'int main() { return 1<=1; }'
check 1  'int main() { return 2>1; }'
check 1  'int main() { return 1>=1; }'

# --- logical and bitwise
check 0  'int main() { return 0&&1; }'
check 1  'int main() { return 1&&1; }'
check 1  'int main() { return 0||1; }'
check 0  'int main() { return 0||0; }'
check 1  'int main() { return !0; }'
check 0  'int main() { return !3; }'
check 8  'int main() { return 12&10; }'
check 14 'int main() { return 12|10; }'
check 6  'int main() { return 12^10; }'
check 48 'int main() { return 12<<2; }'
check 3  'int main() { return 12>>2; }'
check 5  'int main() { return ~-6; }'

# short-circuit must not evaluate the right side
check 1  'int side; int bump() { side = 1; return 1; } int main() { 0 && bump(); return side == 0; }'
check 1  'int side; int bump() { side = 1; return 1; } int main() { 1 || bump(); return side == 0; }'

# --- ternary and comma
check 2  'int main() { return 1 ? 2 : 3; }'
check 3  'int main() { return 0 ? 2 : 3; }'
check 3  'int main() { return (1, 2, 3); }'

# --- variables
check 3  'int main() { int a; a=3; return a; }'
check 8  'int main() { int a; int z; a=3; z=5; return a+z; }'
check 6  'int main() { int a=3; int b=3; return a+b; }'
check 3  'int main() { int foo=3; return foo; }'
check 8  'int main() { int foo123=3; int bar=5; return foo123+bar; }'
check 10 'int main() { int a=1,b=2,c=3,d=4; return a+b+c+d; }'

# --- compound assignment and inc/dec
check 7  'int main() { int a=3; a+=4; return a; }'
check 3  'int main() { int a=7; a-=4; return a; }'
check 12 'int main() { int a=3; a*=4; return a; }'
check 2  'int main() { int a=8; a/=4; return a; }'
check 1  'int main() { int a=9; a%=4; return a; }'
check 4  'int main() { int a=3; return ++a; }'
check 3  'int main() { int a=3; return a++; }'
check 4  'int main() { int a=3; a++; return a; }'
check 2  'int main() { int a=3; return --a; }'
check 3  'int main() { int a=3; return a--; }'

# --- control flow
check 3  'int main() { if (0) return 2; return 3; }'
check 2  'int main() { if (1) return 2; return 3; }'
check 2  'int main() { if (1-1) return 1; else return 2; }'
check 55 'int main() { int i=0; int j=0; for (i=0; i<=10; i=i+1) j=i+j; return j; }'
check 10 'int main() { int i=0; while(i<10) i=i+1; return i; }'
check 5  'int main() { int i=0; do { i=i+1; } while (i<5); return i; }'
check 3  'int main() { int i=0; for(;;) { i=i+1; if (i==3) break; } return i; }'
check 8  'int main() { int s=0; int i; for(i=0;i<5;i=i+1) { if (i==2) continue; s=s+i; } return s; }'
check 1  'int main() { int i=0; while(1) { i=i+1; if (i>0) break; } return i; }'
check 3  'int main() { {1; {2;} return 3;} }'
check 5  'int main() { ;;; return 5; }'

# --- functions
check 3  'int ret3() { return 3; } int main() { return ret3(); }'
check 8  'int add2(int x, int y) { return x+y; } int main() { return add2(3,5); }'
check 2  'int sub2(int x, int y) { return x-y; } int main() { return sub2(5,3); }'
check 21 'int add6(int a,int b,int c,int d,int e,int f) { return a+b+c+d+e+f; } int main() { return add6(1,2,3,4,5,6); }'
check 55 'int fib(int n) { if (n<=1) return n; return fib(n-1)+fib(n-2); } int main() { return fib(10); }'
check 120 'int fact(int n) { if (n==0) return 1; return n*fact(n-1); } int main() { return fact(5); }'
# argument order must survive the register shuffle
check 1  'int ordered(int a,int b,int c,int d,int e,int f) { return a==1&&b==2&&c==3&&d==4&&e==5&&f==6; } int main() { return ordered(1,2,3,4,5,6); }'
# falling off the end of main returns 0
check 0  'int main() { int x=1; }'

# --- pointers
check 3  'int main() { int x=3; return *&x; }'
check 3  'int main() { int x=3; int *y=&x; return *y; }'
check 5  'int main() { int x=3; int *y=&x; *y=5; return x; }'
check 7  'int main() { int x=3; int y=4; int *p=&x; return *p + y; }'
check 8  'int main() { int x=3; int *p=&x; *p+=5; return x; }'
check 4  'int main() { int x; return sizeof(x); }'
check 8  'int main() { int *p; return sizeof(p); }'
check 1  'int main() { char c; return sizeof(c); }'
check 8  'int main() { return sizeof(long); }'
check 4  'int main() { return sizeof(int); }'
check 8  'int main() { return sizeof(int*); }'

# --- arrays: a[i] must mean *(a+i), including the scaling
check 3  'int main() { int a[2]; *a=3; return *a; }'
check 7  'int main() { int a[2]; *a=3; *(a+1)=4; return *a + *(a+1); }'
check 3  'int main() { int a[3]; a[0]=3; return a[0]; }'
check 6  'int main() { int a[3]; a[0]=1; a[1]=2; a[2]=3; return a[0]+a[1]+a[2]; }'
check 12 'int main() { int a[3]; return sizeof(a); }'
check 4  'int main() { int a[4]; return sizeof(a[0])*1 + 0; }'
check 10 'int main() { int a[4]; int i; int s=0; for(i=0;i<4;i=i+1) a[i]=i+1; for(i=0;i<4;i=i+1) s=s+a[i]; return s; }'
# pointer difference is in elements, not bytes
check 3  'int main() { int a[5]; int *p=a; int *q=a+3; return q-p; }'
# char arrays step by 1
check 2  'int main() { char a[4]; a[0]=1; a[1]=1; return a[0]+a[1]; }'

# --- globals
check 3  'int g; int main() { g=3; return g; }'
check 7  'int g1; int g2; int main() { g1=3; g2=4; return g1+g2; }'
check 6  'int ga[4]; int main() { ga[0]=1; ga[1]=2; ga[2]=3; return ga[0]+ga[1]+ga[2]; }'
check 4  'int g; int main() { return sizeof(g); }'

# --- char and string
check 97 'int main() { char c; c = 97; return c; }'
check 65 'int main() { char c = 65; return c; }'
check 10 'int main() { char c = 10; return c; }'
check 97 'int main() { char *s = "abc"; return s[0]; }'
check 98 'int main() { char *s = "abc"; return s[1]; }'
check 0  'int main() { char *s = "abc"; return s[3]; }'
check 4  'int main() { return sizeof("abc"); }'
check 9  'int main() { char *s = "\t"; return s[0]; }'

# --- calling into libc (exercises the ABI against real code)
check 0  'int main() { printf("libc works: %d\n", 42); return 0; }'
check 5  'int main() { return strlen("hello"); }'

check 5   'int main() { int x=0; switch(2) { case 1: x=3; break; case 2: x=5; break; default: x=9; } return x; }'
check 9   'int main() { int x=0; switch(7) { case 1: x=3; break; default: x=9; } return x; }'
check 0   'int main() { int x=0; switch(7) { case 1: x=3; break; } return x; }'
check 8   'int main() { int x=0; switch(1) { case 1: x=x+3; case 2: x=x+5; break; case 3: x=100; } return x; }'
check 4   'int main() { int s=0; int i; for(i=0;i<3;i=i+1) { switch(i) { case 0: s=s+1; break; default: break; } s=s+1; } return s; }'
check 2   'int main() { int i=0; int s=0; while(1) { i=i+1; switch(i) { case 3: break; default: s=s+1; } if (i>=3) break; } return s; }'
check 3   'int main() { int s=0; int i; for(i=0;i<10;i=i+1) { if (i==3) goto done; s=s+i; } done: return s; }'
check 4   'int main() { int i=0; loop: i=i+1; if (i<4) goto loop; return i; }'
check 1   'int main() { goto skip; return 9; skip: return 1; }'
check 1   'int main() { switch(1) { case 1: { int y=1; return y; } } return 0; }'
check 2   'int main() { int x=0; switch(3) { case 1: case 2: x=1; break; case 3: case 4: x=2; break; } return x; }'
check 7   'struct P { int x; int y; }; int main() { struct P p; p.x=3; p.y=4; return p.x+p.y; }'
check 8   'struct P { int x; int y; }; int main() { struct P p; struct P *q=&p; q->x=3; q->y=5; return q->x+q->y; }'
check 8   'struct P { int x; int y; }; int main() { return sizeof(struct P); }'
check 16  'struct M { char c; long l; }; int main() { return sizeof(struct M); }'
check 8   'struct M { char a; char b; int c; }; int main() { return sizeof(struct M); }'
check 8   'union U { int i; long l; }; int main() { return sizeof(union U); }'
check 1   'union U { int i; char c; }; int main() { union U u; u.i=1; return u.c; }'
check 3   'typedef int myint; int main() { myint x = 3; return x; }'
check 9   'typedef struct { int a; int b; } Pair; int main() { Pair p; p.a=4; p.b=5; return p.a+p.b; }'
check 2   'enum E { A, B, C }; int main() { return C - A; }'
check 7   'enum E { A=5, B=7 }; int main() { return B; }'
check 3   'enum E { A=1, B, C }; int main() { return C; }'
check 1   'int main() { int x=1; { int x=2; } return x; }'
check 2   'int main() { int x=1; { int x=2; return x; } }'
check 20  'int main() { int i; int s=0; for (i=0;i<2;i=i+1) { int i=9; s=s+i; } return s+i; }'
check 1   'int main() { unsigned int u = 1; return u; }'
check 0   'int main() { unsigned int u = 0; u = u - 1; return u < 0; }'
check 1   'int main() { unsigned char c = 200; return c > 100; }'
check 5   'struct P { int x; }; int main() { struct P a; struct P b; a.x=5; b=a; return b.x; }'
check 6   'struct N { int v; struct N *next; }; int main() { struct N a; struct N b; a.v=2; b.v=4; a.next=&b; return a.v + a.next->v; }'
check 3   'struct P { int x; int y; }; int main() { struct P a[2]; a[0].x=1; a[1].x=2; return a[0].x+a[1].x; }'
check 30  'struct I { int a; struct { int b; int c; } in; }; int main() { struct I s; s.a=10; s.in.b=20; return s.a+s.in.b; }'
check 1   'struct F { int a; }; int f(struct F *p) { return p->a; } int main() { struct F s; s.a=1; return f(&s); }'
check 12  'typedef struct N N; struct N { int v; }; int main() { N n; n.v=12; return n.v; }'
check 20  'struct L { int v; struct L *next; }; int sum(struct L *p) { int s=0; while (p) { s += p->v; p = p->next; } return s; } int main() { struct L a; struct L b; struct L c; a.v=2; b.v=6; c.v=12; a.next=&b; b.next=&c; c.next=0; return sum(&a); }'
check 1   'int main() { return (char)300 == 44; }'
check 1   'int main() { return (char)-1 == -1; }'
check 1   'int main() { return (unsigned char)-1 == 255; }'
check 1   'int main() { return (short)65537 == 1; }'
check 1   'int main() { return (_Bool)5 == 1; }'
check 1   'int main() { long l = -1; return (int)l == -1; }'
check 1   'int main() { int i=-1; long l=(long)i; return l == -1; }'
check 1   'int main() { unsigned int u = 4294967295; long l = (long)u; return l == 4294967295; }'
check 5   'int main() { int a[3]; char *p = (char *)a; p[0]=5; return a[0]; }'
check 1   'int main() { return (int)(char)(short)1 == 1; }'
check 12  'int main() { return sizeof(int[3]); }'
check 2   'int main() { return -(char)-2; }'
#__APPEND_BELOW__
printf '\n%d passed, %d failed\n' "$pass" "$fail"
rm -f t/tmp.c t/tmp t/tmp.err
[ "$fail" = 0 ]
