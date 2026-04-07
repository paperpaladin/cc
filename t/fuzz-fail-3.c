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
  unsigned long v0 = ((((2601030205UL <= 2015337560UL) || (3080127366UL >= 2360647895UL)) ? 3698061793UL : 3338197566UL) - (2538753386UL % ((3717411168UL) | 1UL)));
  unsigned int n0 = (unsigned int)((1833671387UL ^ 4018314376UL) & 0xFFFFUL);
  acc = acc * 31UL + ((unsigned long)(long)n0);
  acc = acc * 31UL + ((unsigned long)widen(n0, n0));
  unsigned long result = acc ^ (v0);
  return (int)(result % 251UL);
}
