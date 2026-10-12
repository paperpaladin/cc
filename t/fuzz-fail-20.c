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
  unsigned long v0 = (((727473361UL != 1767478100UL) ? 4268501763UL : 443445945UL) / (((1930161564UL - 858203856UL)) | 1UL));
  struct P s0; s0.a = (v0 | v0); s0.b = v0;
  acc = acc * 31UL + (fold(s0));
  unsigned long result = acc ^ (v0);
  return (int)(result % 251UL);
}
