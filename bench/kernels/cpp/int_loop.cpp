#include <stdio.h>
int kernel(int n){unsigned s=0;for(int i=1;i<=n;i++){s=s+(unsigned)(i*(i%7))-(unsigned)(i>>3);if((i&1023)==0)s^=(unsigned)i;}return (int)s;}
int main(){printf("%d\n",kernel(30000000));return 0;}
