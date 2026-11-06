function P(){this.a=1;this.b=2;this.c=3;this.d=4;}
function kernel(n){var o=new P();for(var i=0;i<n;i++){o.a=o.b+1;o.b=o.c+1;o.c=o.d-1;o.d=o.a-1;}return o.a+o.b+o.c+o.d;}
console.log(kernel(10000000));
