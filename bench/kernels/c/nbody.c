#include <stdio.h>
#include <math.h>
struct B{double x,y,z,vx,vy,vz,m;};
struct B bs[5];
void setb(int i,double x,double y,double z,double vx,double vy,double vz,double m){bs[i].x=x;bs[i].y=y;bs[i].z=z;bs[i].vx=vx;bs[i].vy=vy;bs[i].vz=vz;bs[i].m=m;}
void adv(double dt){for(int i=0;i<5;i++){for(int j=i+1;j<5;j++){double dx=bs[i].x-bs[j].x,dy=bs[i].y-bs[j].y,dz=bs[i].z-bs[j].z;double d2=dx*dx+dy*dy+dz*dz;double mag=dt/(d2*sqrt(d2));bs[i].vx-=dx*bs[j].m*mag;bs[i].vy-=dy*bs[j].m*mag;bs[i].vz-=dz*bs[j].m*mag;bs[j].vx+=dx*bs[i].m*mag;bs[j].vy+=dy*bs[i].m*mag;bs[j].vz+=dz*bs[i].m*mag;}}for(int i=0;i<5;i++){bs[i].x+=dt*bs[i].vx;bs[i].y+=dt*bs[i].vy;bs[i].z+=dt*bs[i].vz;}}
int main(){setb(0,0,0,0,0,0,0,39.47);setb(1,4.84,-1.16,-0.10,0.606,2.81,-0.02,0.037);setb(2,8.34,4.12,-0.4,-1.01,1.82,0.008,0.011);setb(3,12.89,-15.11,-0.22,1.08,0.868,-0.01,0.0017);setb(4,15.37,-25.9,0.179,0.979,0.594,-0.034,0.002);for(int s=0;s<500000;s++)adv(0.01);printf("%.0f\n",floor(bs[0].x*1e6+0.5));return 0;}
