class V{constructor(x,y){this.x=x;this.y=y;}add(o){return this.x*o.x+this.y*o.y;}get m(){return this.x;}}
class W extends V{add(o){return super.add(o)+1;}}
function kernel(n){var a=new V(1,2),b=new W(3,4),s=0;for(var i=0;i<n;i++){s+=a.add(b);s+=b.add(a);s+=a.m;}return s;}
console.log(kernel(3000000));
