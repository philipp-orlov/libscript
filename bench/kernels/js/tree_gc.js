function T(l,r){this.l=l;this.r=r;}
function mk(d){return d===0?new T(null,null):new T(mk(d-1),mk(d-1));}
function chk(t){return t.l===null?1:1+chk(t.l)+chk(t.r);}
function kernel(){var long=mk(16),s=0;for(var d=4;d<=16;d+=4){var it=1<<(16-d+4);for(var i=0;i<it;i++)s+=chk(mk(d));}return s+chk(long);}
console.log(kernel());
