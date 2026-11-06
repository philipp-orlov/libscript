using System;
static class K { public static int Fib(int n){return n<2?n:Fib(n-1)+Fib(n-2);} }
Console.WriteLine(K.Fib(35));
