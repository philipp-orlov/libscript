using System;
static class K { public static int Kernel(int n,int rounds){int c=0;for(int r=0;r<rounds;r++){var a=new int[n];for(int i=0;i<n;i++)a[i]=1;c=0;for(int i=2;i<n;i++){if(a[i]!=0){c++;for(int j=i*2;j<n;j+=i)a[j]=0;}}}return c;} }
Console.WriteLine(K.Kernel(2000000,4));
