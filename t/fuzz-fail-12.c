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
  unsigned long v0 = ((1639135743UL * 2072488418UL) ^ (2397408006UL << ((1890020937UL) & 63UL)));
  unsigned long v1 = ((v0 ^ v0) + (v0 << ((3503394736UL) & 63UL)));
  v1 = (((v1 * 1331021629UL) * v0) + ((v1 - 2362109041UL) << ((v0) & 63UL)));
  unsigned char n0 = (unsigned char)((v1 / ((v1) | 1UL)) & 0xFFUL);
  acc = acc * 31UL + ((unsigned long)(long)n0);
  acc = acc * 31UL + ((unsigned long)widen(n0, n0));
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
