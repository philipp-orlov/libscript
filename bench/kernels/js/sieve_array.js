function kernel(n,rounds){var c=0;for(var r=0;r<rounds;r++){var a=new Array(n);for(var i=0;i<n;i++)a[i]=1;c=0;for(var i=2;i<n;i++){if(a[i]){c++;for(var j=i*2;j<n;j+=i)a[j]=0;}}}return c;}
console.log(kernel(2000000,4));
