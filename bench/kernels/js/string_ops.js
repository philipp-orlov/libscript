function kernel(n){var t=0;for(var r=0;r<n;r++){var s="";for(var i=0;i<2000;i++)s+=String.fromCharCode(97+i%26);var h=0;for(var i=0;i<s.length;i++)h=(h*31+s.charCodeAt(i))|0;var parts=s.split("a");t=(t+h+parts.join("-").length+s.indexOf("xyz")+s.substring(5,50).toUpperCase().length)|0;}return t;}
console.log(kernel(1500));
