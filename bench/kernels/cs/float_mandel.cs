using System;
static class K {
 public static int Esc(double cx,double cy,int lim){double x=0,y=0;int i=0;while(i<lim&&x*x+y*y<=4.0){double t=x*x-y*y+cx;y=2.0*x*y+cy;x=t;i++;}return i;}
 public static int Kernel(){int s=0;for(int py=0;py<400;py++)for(int px=0;px<400;px++)s+=Esc(-2.0+px*3.0/400.0,-1.5+py*3.0/400.0,100);return s;}
}
Console.WriteLine(K.Kernel());
