struct P { unsigned long a, b; };
unsigned long mix(unsigned long a, unsigned long b) {
  return (a * 3UL) ^ (b + 7UL) ^ (a >> 3UL);
}
long narrow(signed char a, unsigned short b) {
  return (long)a * 3L + (long)b;
}
long widen(long a, long b) {
  return a * 7L - b;
}
unsigned long fold(struct P p) {
  return (p.a ^ (p.b * 5UL)) + (p.a >> 7UL);
}
int main(void) {
  unsigned long acc = 0UL;
  unsigned long v0 = ((815623033UL & 799609893UL) >> (((609005468UL & 4218488628UL)) & 63UL));
  unsigned long v1 = (v0 / ((v0) | 1UL));
  unsigned long v2 = ((v0 / ((v1) | 1UL)) % ((v1) | 1UL));
  v2 = mix(v1, v0);
  acc = acc * 31UL + ((unsigned long)narrow(v1, v0));
  unsigned long v3 = (v0 | v0);
  v1 = mix(v0, v3);
  acc = acc * 31UL + ((unsigned long)narrow(v0, v3));
  unsigned long result = acc ^ (v0 ^ v1 ^ v2 ^ v3);
  return (int)(result % 251UL);
}
