using System;
class B { public double x,y,z,vx,vy,vz,m; public B(double x,double y,double z,double vx,double vy,double vz,double m){this.x=x;this.y=y;this.z=z;this.vx=vx;this.vy=vy;this.vz=vz;this.m=m;} }
static class K {
 public static void Adv(B[] bs,double dt){int n=bs.Length;for(int i=0;i<n;i++){var a=bs[i];for(int j=i+1;j<n;j++){var b=bs[j];double dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;double d2=dx*dx+dy*dy+dz*dz;double mag=dt/(d2*Math.Sqrt(d2));a.vx-=dx*b.m*mag;a.vy-=dy*b.m*mag;a.vz-=dz*b.m*mag;b.vx+=dx*a.m*mag;b.vy+=dy*a.m*mag;b.vz+=dz*a.m*mag;}}for(int i=0;i<n;i++){var c=bs[i];c.x+=dt*c.vx;c.y+=dt*c.vy;c.z+=dt*c.vz;}}
 public static double Kernel(int steps){var bs=new B[]{new B(0,0,0,0,0,0,39.47),new B(4.84,-1.16,-0.10,0.606,2.81,-0.02,0.037),new B(8.34,4.12,-0.4,-1.01,1.82,0.008,0.011),new B(12.89,-15.11,-0.22,1.08,0.868,-0.01,0.0017),new B(15.37,-25.9,0.179,0.979,0.594,-0.034,0.002)};for(int s=0;s<steps;s++)Adv(bs,0.01);return Math.Round(bs[0].x*1e6);}
}
Console.WriteLine(K.Kernel(500000));
