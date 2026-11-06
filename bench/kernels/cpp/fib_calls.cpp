#include <stdio.h>
int fib(int n){return n<2?n:fib(n-1)+fib(n-2);}
int main(){printf("%d\n",fib(35));return 0;}
