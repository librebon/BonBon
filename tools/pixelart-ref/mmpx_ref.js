// Runs runMMPX2X from McGuire & Gagiu's js-demo.html (MMPX supplement) on a raw image.
// usage: node mmpx_ref.js <js-demo.html> <in.raw> <out.raw>
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const start = html.indexOf('function luma(C)');
const end = html.indexOf('function runNearest2X');
const code = html.slice(start, end);
const runMMPX2X = new Function(code + '\nreturn runMMPX2X;')();
const buf = fs.readFileSync(process.argv[3]);
const w = buf.readUInt32LE(0), h = buf.readUInt32LE(4);
const src = new Uint32Array(w * h);
for (let i = 0; i < w * h; ++i) src[i] = buf.readUInt32LE(8 + 4 * i);
const dst = new Uint32Array(4 * w * h);
runMMPX2X(src, w, h, dst);
const out = Buffer.alloc(8 + 16 * w * h);
out.writeUInt32LE(2 * w, 0); out.writeUInt32LE(2 * h, 4);
for (let i = 0; i < dst.length; ++i) out.writeUInt32LE(dst[i] >>> 0, 8 + 4 * i);
fs.writeFileSync(process.argv[4], out);
