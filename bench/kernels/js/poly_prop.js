var os=[{x:1},{y:0,x:2},{z:0,y:0,x:3},{w:0,z:0,y:0,x:4}];
function kernel(n){var s=0;for(var i=0;i<n;i++){s+=os[i&3].x;}return s;}
console.log(kernel(20000000));
