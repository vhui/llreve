/*@ opt -nest @*/
extern int __mark(int);
int f(int n) {
    int i = 1;
    int x = 1;

    while (__mark(1) & i <= n) {
        x = x * 5;
        i++;
    }

    i = 0;

    while (__mark(2) & i <= n) {
        x = x + i;
        i++;
    }
    return x;
}
