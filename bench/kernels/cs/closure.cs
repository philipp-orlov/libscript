using System;
static class K {
 static Func<int,int> Mk(){int c=0;return d=>{c+=d;return c;};}
 public static int Kernel(int n){var f=Mk();var g=Mk();int s=0;for(int i=0;i<n;i++){s=f(i&1)+g(1);}return s;}
}
Console.WriteLine(K.Kernel(10000000));
