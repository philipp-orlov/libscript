using System;
class T { public T l,r; public T(T l,T r){this.l=l;this.r=r;} }
static class K {
 public static T Mk(int d){return d==0?new T(null,null):new T(Mk(d-1),Mk(d-1));}
 public static int Chk(T t){return t.l==null?1:1+Chk(t.l)+Chk(t.r);}
 public static int Kernel(){var lg=Mk(16);int s=0;for(int d=4;d<=16;d+=4){int it=1<<(16-d+4);for(int i=0;i<it;i++)s+=Chk(Mk(d));}return s+Chk(lg);}
}
Console.WriteLine(K.Kernel());
