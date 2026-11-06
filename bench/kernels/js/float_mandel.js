function esc(cx,cy,lim){var x=0,y=0,i=0;while(i<lim&&x*x+y*y<=4){var t=x*x-y*y+cx;y=2*x*y+cy;x=t;i++;}return i;}
function kernel(){var s=0;for(var py=0;py<400;py++)for(var px=0;px<400;px++)s+=esc(-2+px*3/400,-1.5+py*3/400,100);return s;}
console.log(kernel());
