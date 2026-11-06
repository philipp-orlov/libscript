#include <stdio.h>
#include <stdlib.h>
int kernel(int n,int rounds){int c=0;for(int r=0;r<rounds;r++){int* a=(int*)malloc(sizeof(int)*n);for(int i=0;i<n;i++)a[i]=1;c=0;for(int i=2;i<n;i++){if(a[i]){c++;for(int j=i*2;j<n;j+=i)a[j]=0;}}free(a);}return c;}
int main(){printf("%d\n",kernel(2000000,4));return 0;}
