using System;
class V { public double x,y; public V(double x,double y){this.x=x;this.y=y;} public virtual double Add(V o){return x*o.x+y*o.y;} public double M{get{return x;}} }
class W : V { public W(double x,double y):base(x,y){} public override double Add(V o){return base.Add(o)+1;} }
static class K { public static double Kernel(int n){V a=new V(1,2);V b=new W(3,4);double s=0;for(int i=0;i<n;i++){s+=a.Add(b);s+=b.Add(a);s+=a.M;}return s;} }
Console.WriteLine(K.Kernel(3000000));
