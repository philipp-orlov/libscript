// Typed arrays, DataView, bit operations, Math.
function mix(words, rounds) { var h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a]; for (var r = 0; r < rounds; r++) for (var i = 0; i < words.length; i++) { var w = words[i]; h[0] = (h[0] + ((w << 7) | (w >>> 25)) ^ h[1]) | 0; h[1] = (h[1] ^ (h[2] >>> 3)) + (w & h[3]) | 0; h[2] = Math.imul(h[2] ^ w, 0x9e3779b1); h[3] = (h[3] + h[0]) >>> 0; } return (h[0] ^ h[1] ^ h[2] ^ h[3]) >>> 0; }
var total = 0;
for (var round = 0; round < 30; round++) {
  var buf = new ArrayBuffer(4096), u8 = new Uint8Array(buf), u32 = new Uint32Array(buf), f64 = new Float64Array(buf, 0, 256), dv = new DataView(buf);
  for (var i = 0; i < u8.length; i++) u8[i] = (i * 31 + round) & 255;
  total += mix(u32, 4) % 1000;
  for (var i = 0; i < 500; i++) { dv.setUint16(i * 2, i, i % 2 == 0); dv.setFloat32(1024 + i * 4, i / 3, true); total += dv.getInt16(i * 2, false) & 3; total += dv.getFloat64((i % 100) * 8, true) > 0 ? 1 : 0; }
  var f = new Float64Array(2000); for (var i = 0; i < f.length; i++) f[i] = Math.sin(i * 0.01) * Math.cos(i * 0.02) + Math.sqrt(i) / (1 + Math.abs(Math.tan(i * 0.001)));
  var s = 0; for (var i = 1; i < f.length - 1; i++) s += (f[i - 1] + 2 * f[i] + f[i + 1]) * 0.25 + Math.floor(f[i] * 10) + Math.round(f[i]) + Math.pow(f[i], 2) + Math.atan2(f[i], 1) + Math.log(i) + Math.exp(-i * 0.001) + Math.hypot(f[i], 1) + (f[i] | 0) + Math.min(f[i], 0.5) + Math.trunc(f[i] * 3) + Math.sign(f[i]) + Math.cbrt(i) + Math.tanh(f[i]);
  total += s | 0;
  var sub = u8.subarray(100, 200), copy = u8.slice(0, 64), i16 = new Int16Array([5, -3, 9, 1]); u8.set(copy, 300); i16.sort(); u8.fill(7, 3000, 3100);
  total += sub.length + copy[10] + i16[0] + u8.indexOf(7) + new Uint8Array([1, 2, 3]).reduce(function (a, b) { return a + b; }, 0) + Array.from(new Uint16Array(u8.buffer, 0, 8)).join('').length + new Uint8ClampedArray([300, -5])[0];
  var bits = 0; for (var i = 0; i < 20000; i++) { var x = i * 2654435761 >>> 0; x ^= x >>> 16; x = Math.imul(x, 0x85ebca6b); x ^= x >>> 13; bits += (x & 1) + ((x >>> 31) ^ (i & 1)) + Math.clz32(x & 0xffff); }
  total += bits & 0xffff;
}
console.log(total);
