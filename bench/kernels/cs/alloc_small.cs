using System;
class N { public int v; public N next; public int[] k; }
static class K { public static int Kernel(int n){N last=null;int s=0;for(int i=0;i<n;i++){var o=new N();o.v=i;o.k=new int[1];o.k[0]=i;if(i%16==0)last=null;o.next=last;last=o;s+=o.k[0]&1;}return s;} }
Console.WriteLine(K.Kernel(4000000));
