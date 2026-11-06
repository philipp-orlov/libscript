using System;
class P { public double a=1,b=2,c=3,d=4; }
static class K { public static double Kernel(int n){var o=new P();for(int i=0;i<n;i++){o.a=o.b+1;o.b=o.c+1;o.c=o.d-1;o.d=o.a-1;}return o.a+o.b+o.c+o.d;} }
Console.WriteLine(K.Kernel(10000000));
