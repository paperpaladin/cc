#!/bin/sh
# Append a case to t/run.sh with its expected value taken from gcc, not from me.
#
#   ./t/addcase.sh 'int main() { return 1+1; }'
#
# Three of my hand-computed expectations have been wrong so far, every one of them
# costing a detour through "is this a codegen bug?". gcc already knows the answer,
# so stop guessing: compile with gcc, run it, record what it returns. clang is
# consulted too -- if the two disagree the program is relying on undefined
# behaviour and has no business in the suite.
cd "$(dirname "$0")/.." || exit 1

src="$1"
[ -n "$src" ] || { echo "usage: $0 'C PROGRAM'"; exit 2; }

printf '%s' "$src" > t/add.c

if ! gcc -w t/add.c -o t/add.bin 2>t/add.err; then
  echo "gcc rejects this program:"
  sed 's/^/    /' t/add.err
  rm -f t/add.c t/add.err
  exit 1
fi
./t/add.bin; g=$?

if clang -w t/add.c -o t/add.bin 2>/dev/null; then
  ./t/add.bin; c=$?
  if [ "$g" != "$c" ]; then
    echo "gcc says $g but clang says $c -- undefined behaviour, not adding"
    rm -f t/add.c t/add.bin t/add.err
    exit 1
  fi
fi

# Single-quote the program for run.sh; refuse rather than mangle one containing a
# single quote, since escaping it correctly inside a single-quoted shell word is
# not worth the subtlety.
case "$src" in
  *\'*) echo "program contains a single quote; add it to t/run.sh by hand"; exit 1 ;;
esac

# Insert above the marker, so the summary and cleanup stay at the end of run.sh.
line=$(printf "check %-3s '%s'" "$g" "$src")
awk -v new="$line" '/^#__APPEND_BELOW__$/ { print new } { print }' t/run.sh > t/run.new \
  && mv t/run.new t/run.sh && chmod +x t/run.sh
echo "added: expected $g (gcc and clang agree)"
rm -f t/add.c t/add.bin t/add.err
