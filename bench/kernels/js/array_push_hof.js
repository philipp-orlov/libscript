function kernel(rounds){var t=0;for(var r=0;r<rounds;r++){var a=[];for(var i=0;i<100000;i++)a.push(i);t+=a.map(function(x){return x*2;}).filter(function(x){return x%3===0;}).reduce(function(p,x){return p+x;},0);while(a.length)a.pop();}return t;}
console.log(kernel(10));
