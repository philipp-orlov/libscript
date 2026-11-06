function kernel(n){var m=new Map(),o={},s=0;for(var i=0;i<n;i++){var k="k"+(i&4095);m.set(k,(m.get(k)||0)+1);o[k]=(o[k]||0)+1;m.set(i&65535,i);s+=m.get(i&65535)&1;}return s+m.size+Object.keys(o).length;}
console.log(kernel(2000000));
