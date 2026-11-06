using System;
using System.Text;
static class K { public static int Kernel(int n){int t=0;for(int r=0;r<n;r++){var b=new StringBuilder();for(int i=0;i<2000;i++)b.Append((char)(97+i%26));var s=b.ToString();int h=0;for(int i=0;i<s.Length;i++)h=h*31+(int)s[i];var parts=s.Split('a');t=t+h+string.Join("-",parts).Length+s.IndexOf("xyz")+s.Substring(5,45).ToUpper().Length;}return t;} }
Console.WriteLine(K.Kernel(1500));
