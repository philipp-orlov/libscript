// Strings: building, cutting, searching, case, numbers into text and back.
function words(n) { var out = [], x = 7; for (var i = 0; i < n; i++) { x = (x * 1103515245 + 12345) & 0x7fffffff; out.push('w' + (x % 977).toString(36) + (i % 13 == 0 ? 'Zz' : '')); } return out; }
var w = words(4000), total = 0;
for (var round = 0; round < 25; round++) {
  var text = w.join(' ');
  var parts = text.split(' ');
  total += parts.length + text.indexOf('wzz') + text.lastIndexOf('Zz') + (text.includes('w1a') ? 1 : 0);
  var up = '', acc = 0;
  for (var i = 0; i < parts.length; i += 3) {
    var p = parts[i];
    up += p.toUpperCase().slice(0, 3) + p.charAt(p.length - 1) + ',';
    acc += p.charCodeAt(1) + p.length + (p.startsWith('w1') ? 2 : 0) + (p.endsWith('Zz') ? 3 : 0);
    if (i % 30 == 0) up += String(i).padStart(6, '0') + (i / 7).toFixed(2) + i.toString(16) + parseInt(p.slice(1), 36) + parseFloat('3.25e' + (i % 5));
  }
  total += up.length + acc;
  var rep = text.replace(/w([0-9a-z])/g, function (m, c) { return c + '-'; }).replace(/\s+/g, '_').substring(100, 5000).trim();
  total += rep.length + rep.split('_').map(function (s) { return s.length; }).reduce(function (a, b) { return a + b; }, 0);
  var tpl = '';
  for (var i = 0; i < 300; i++) tpl += `<li class="${parts[i]}" data-n=${i}>${parts[i + 1]} ${i * 1.5}</li>`;
  total += tpl.length + [...'héllo wörld ✓'].reverse().join('').normalize('NFC').length + 'a,b;c'.split(/[,;]/).length + escape('a b').length + encodeURIComponent('ä b/c').length;
  var cmp = parts.slice(0, 500).sort().slice(0, 5).join('') + parts.slice(0, 200).sort(function (a, b) { return a < b ? 1 : a > b ? -1 : 0; })[0];
  total += cmp.length + cmp.localeCompare('w') + JSON.stringify(parts.slice(0, 50)).length;
}
console.log(total);
