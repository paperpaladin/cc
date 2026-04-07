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
  unsigned long v0 = ((386977619UL + 3942586889UL) % (((2305023086UL << ((456053774UL) & 63UL))) | 1UL));
  unsigned long v1 = ((v0 + 1068903724UL) >> ((v0) & 63UL));
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
