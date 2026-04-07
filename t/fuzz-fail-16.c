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
  unsigned long v0 = ((25140966UL ^ 3053728006UL) - (43501029UL / ((1331904807UL) | 1UL)));
  for (int i979 = 0; i979 < 5; i979++) {
    v0 = v0;
  unsigned int n0 = (unsigned int)((~1553844406UL) & 0xFFFFUL);
  acc = acc * 31UL + ((unsigned long)(long)n0);
  acc = acc * 31UL + ((unsigned long)widen(n0, n0));
  signed char n1 = (signed char)((((v0 == 3394446673UL) ? v0 : v0) & 0x7FUL) - 64);
  acc = acc * 31UL + ((unsigned long)(long)n1);
  acc = acc * 31UL + ((unsigned long)widen(n1, n1));
  v0 = (((v0 ^ v0) * v0) >> (((v0 << (((1686565977UL ^ v0)) & 63UL))) & 63UL));
  unsigned long v1 = v0;
  }
  unsigned long v1 = v0;
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
