using System;
static class K { public static int Kernel(int n){int s=0;for(int i=1;i<=n;i++){s=s+(i*(i%7))-(i>>3);if((i&1023)==0)s^=i;}return s;} }
Console.WriteLine(K.Kernel(30000000));
