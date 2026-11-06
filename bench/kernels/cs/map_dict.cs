using System;
using System.Collections.Generic;
static class K { public static int Kernel(int n){var m=new Dictionary<string,int>();var o=new Dictionary<string,int>();var q=new Dictionary<int,int>();int s=0;for(int i=0;i<n;i++){var k="k"+(i&4095);m[k]=(m.ContainsKey(k)?m[k]:0)+1;o[k]=(o.ContainsKey(k)?o[k]:0)+1;q[i&65535]=i;s+=q[i&65535]&1;}return s+m.Count+q.Count+o.Count;} }
Console.WriteLine(K.Kernel(2000000));
