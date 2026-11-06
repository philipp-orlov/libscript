function mk(){var c=0;return function(d){c+=d;return c;};}
function kernel(n){var f=mk(),g=mk(),s=0;for(var i=0;i<n;i++){s=f(i&1)+g(1);}return s;}
console.log(kernel(10000000));
