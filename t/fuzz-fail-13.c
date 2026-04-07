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
  unsigned long v0 = ((2800360461UL > 2861245199UL) ? (559260700UL << ((1265541136UL) & 63UL)) : (630116043UL * 3549212109UL));
  if (((504057573UL & 4233468453UL) > (579192495UL / ((v0) | 1UL)))) {
  v0 = (((v0 & v0) / (((v0 | v0)) | 1UL)) + v0);
  struct P s0; s0.a = v0; s0.b = (2707969222UL << ((v0) & 63UL));
  acc = acc * 31UL + (fold(s0));
  }
  if (((v0 * v0) >= v0)) {
  unsigned long v1 = ((3544879032UL / ((v0) | 1UL)) / ((v0) | 1UL));
  v1 = (((v0 | v1) - (v0 ^ v1)) / ((((2543009388UL | v1) >> (((v0 + v0)) & 63UL))) | 1UL));
  }
  unsigned long result = acc ^ (v0);
  return (int)(result % 251UL);
}
