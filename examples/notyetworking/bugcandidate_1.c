extern int __mark(int);
int g(int n)
{
  int r=0;
  int i=n;

  while (__mark(0) & (i > 0)) {
    r = r + n;
    i--;
  }

  r=0;
  i=n;
  while (__mark(1) & (i > 0)) {
    r = r + n;
    i--;
  }

  return r;
}
