// Regular expressions: a tokenizer with sticky patterns, and the string methods that take a pattern.
var src = '';
for (var i = 0; i < 400; i++) src += 'let v' + i + ' = (a' + i + ' + 0x1F * 3.5e2) / "str\\"ing' + i + '" // note ' + i + '\n  if (v' + i + ' >= 10 && !done) { call_' + i + '(v' + i + ', \'x\'); }\n';
var rules = [['ws', /[ \t\n]+/y], ['comment', /\/\/[^\n]*/y], ['num', /0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:e[+-]?\d+)?/y], ['str', /"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'/y], ['id', /[A-Za-z_$][\w$]*/y], ['op', />=|<=|&&|\|\||[-+*\/=(){};,!<>]/y]];
function tokenize(text) { var pos = 0, out = []; while (pos < text.length) { var hit = false; for (var r = 0; r < rules.length; r++) { var re = rules[r][1]; re.lastIndex = pos; var m = re.exec(text); if (m) { if (r > 1) out.push(rules[r][0] + ':' + m[0]); pos = re.lastIndex; hit = true; break; } } if (!hit) pos++; } return out; }
var total = 0;
for (var round = 0; round < 12; round++) {
  var toks = tokenize(src);
  total += toks.length;
  total += (src.match(/v\d+/g) || []).length + src.search(/call_3\d/) + src.replace(/(\w+)\s*=\s*/g, '$1:=').length;
  var dates = 'on 2024-03-17 and 1999-12-31, not 20-1-1';
  for (var k = 0; k < 300; k++) { var d = /(?<y>\d{4})-(?<m>\d{2})-(?<d>\d{2})/.exec(dates); total += +d.groups.y + d.index + (/^\s*$/.test(' \t') ? 1 : 0) + (/(a|ab)(c|bcd)(d*)/.test('abcd') ? 1 : 0); }
  for (var m of dates.matchAll(/\d+/g)) total += m[0].length;
  total += 'a1b22c333'.split(/\d+/).length + 'CamelCaseWordsHere'.replace(/([a-z])([A-Z])/g, '$1_$2').toLowerCase().length + 'x'.repeat(50).replace(/x{3}/g, 'y').length;
  total += /^[\w.+-]+@[\w-]+\.[\w.]+$/i.test('Some.One+tag@example.org') ? 5 : 0;
}
console.log(total);
