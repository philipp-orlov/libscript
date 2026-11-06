function kernel(n){var d={name:"libscript",tags:["a","b","c"],items:[]};for(var i=0;i<200;i++)d.items.push({id:i,label:"item"+i,w:i*0.5,on:i%2===0});var t=0;for(var r=0;r<n;r++){var o=JSON.parse(JSON.stringify(d));t+=o.items.length+o.items[r%200].id;}return t;}
console.log(kernel(2000));
