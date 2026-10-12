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
  unsigned long v0 = (((3609267711UL < 673671309UL) ? 1596840319UL : 3729944832UL) % (((1070867289UL ^ 930847394UL)) | 1UL));
  v0 = 2651938379UL;
  unsigned long v1 = 4219466551UL;
  unsigned long result = acc ^ (v0 ^ v1);
  return (int)(result % 251UL);
}
