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
  unsigned long v0 = ((1930549411UL ^ 901749037UL) << (((1674216077UL & 1912923437UL)) & 63UL));
  if (((v0 + v0) >= (v0 ^ v0))) {
  unsigned long v1 = ((3979315673UL % ((v0) | 1UL)) - (v0 % ((4154136494UL) | 1UL)));
  unsigned long v2 = ((v1 >= 2170177477UL) ? (v1 | v1) : (v1 % ((v0) | 1UL)));
  v1 = mix(v0, v1);
  acc = acc * 31UL + ((unsigned long)narrow(v0, v1));
  signed char n0 = (signed char)(((2483246605UL ^ v2) & 0x7FUL) - 64);
  acc = acc * 31UL + ((unsigned long)(long)n0);
  acc = acc * 31UL + ((unsigned long)widen(n0, n0));
  unsigned long v3 = v2;
  unsigned long v4 = (v0 | (2411013676UL % ((v0) | 1UL)));
  } else {
  unsigned long a0[3] = {866962UL, 1016961UL, 748242UL};
  acc = acc * 31UL + (a0[1]);
  v0 = a0[((v0 + 2891000577UL)) % 3UL];
  unsigned long v1 = v0;
  }
  unsigned long result = acc ^ (v0);
  return (int)(result % 251UL);
}
