function kernel(n){var last=null,s=0;for(var i=0;i<n;i++){var o={v:i,next:null,k:[i]};if(i%16===0)last=null;o.next=last;last=o;s+=o.k[0]&1;}return s;}
console.log(kernel(4000000));
