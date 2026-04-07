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
  unsigned long v0 = ((2906162014UL * 2159432582UL) & (1627876803UL ^ 4063964356UL));
  unsigned long v1 = (v0 ^ (v0 % ((v0) | 1UL)));
  unsigned long v2 = (v0 | 3807057864UL);
  v1 = mix(v1, v0);
  acc = acc * 31UL + ((unsigned long)narrow(v1, v0));
  for (int i142 = 0; i142 < 1; i142++) {
    v0 = ((16641381UL <= 3660155614UL) ? (v0 << ((v2) & 63UL)) : (((v2 <= 624304388UL) && (v2 < v0)) ? 147051781UL : 3891850392UL));
  struct P s0; s0.a = v2; s0.b = (v1 / ((v2) | 1UL));
  acc = acc * 31UL + (fold(s0));
  v0 = (1411807950UL / ((((v0 / ((v2) | 1UL)) | (991936988UL | v2))) | 1UL));
  }
  v2 = v1;
  unsigned long result = acc ^ (v0 ^ v1 ^ v2);
  return (int)(result % 251UL);
}
