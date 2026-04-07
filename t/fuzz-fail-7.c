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
  unsigned long v0 = ((404285457UL & 2179419893UL) + (1796035739UL - 1823296038UL));
  for (int i126 = 0; i126 < 2; i126++) {
    v0 = ((1243862422UL - v0) & (v0 * v0));
  struct P s0; s0.a = (3966150535UL * v0); s0.b = (v0 * 4209818936UL);
  acc = acc * 31UL + (fold(s0));
  }
  unsigned long v1 = (v0 % (((v0 * v0)) | 1UL));
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
