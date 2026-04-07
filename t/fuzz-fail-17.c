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
  unsigned long v0 = (~(2840618605UL + 3940602561UL));
  unsigned long v1 = ((v0 >> ((v0) & 63UL)) - (2945620625UL * v0));
  v0 = (((v0 ^ v1) * (v0 & v0)) << (((556137866UL ^ (v0 & v1))) & 63UL));
  v0 = (((v0 % ((2082654792UL) | 1UL)) & (v1 / ((v1) | 1UL))) - ((2306302791UL < 3952167747UL) ? (3311272756UL * v1) : (v0 - v1)));
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
