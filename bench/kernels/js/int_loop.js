function kernel(n){var s=0;for(var i=1;i<=n;i++){s=(s+((i*(i%7))|0)-(i>>3))|0;if((i&1023)===0)s^=i;}return s;}
console.log(kernel(30000000));
