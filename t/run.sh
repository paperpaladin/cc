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

printf '\n%d passed, %d failed\n' "$pass" "$fail"
rm -f t/tmp.c t/tmp t/tmp.err
[ "$fail" = 0 ]
