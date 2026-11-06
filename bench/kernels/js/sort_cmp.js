function kernel(n){var a=[],x=12345;for(var i=0;i<n;i++){x=(x*1103515245+12345)&0x7fffffff;a.push(x);}a.sort(function(p,q){return p-q;});var c=0;for(var i=1;i<n;i++)if(a[i-1]<=a[i])c++;return c;}
console.log(kernel(500000));
