// Draws the app icon (the same orrery as the favicon: two rings and a planet) into desktop/icon.png.
// Plain Node, no image libraries: shapes are tested per pixel with 4x4 supersampling, then written as a PNG.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const S = 512, k = S / 64, SS = 4;
const hex = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
const BG = hex('#0a0614'), LIME = hex('#c9f27a'), PINK = hex('#ff7ad9'), CYAN = hex('#3ff0e0');

// what colour (if any) covers a point, in favicon units (0-64)
function sample(x, y) {
  const rr = 14, inX = Math.max(rr - x, 0, x - (64 - rr)), inY = Math.max(rr - y, 0, y - (64 - rr));
  if (Math.hypot(inX, inY) > rr) return null; // outside the rounded square
  const d = Math.hypot(x - 32, y - 32);
  if (Math.hypot(x - 52, y - 32) <= 5) return CYAN;
  if (Math.abs(d - 20) <= 2) return LIME;
  if (Math.abs(d - 9) <= 2) return PINK;
  return BG;
}

const px = Buffer.alloc(S * S * 4);
for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
  let r = 0, g = 0, b = 0, a = 0;
  for (let sy = 0; sy < SS; sy++) for (let sx = 0; sx < SS; sx++) {
    const c = sample((x + (sx + .5) / SS) / k, (y + (sy + .5) / SS) / k);
    if (c) { r += c[0]; g += c[1]; b += c[2]; a++; }
  }
  const i = (y * S + x) * 4, n = SS * SS;
  if (a) { px[i] = r / a; px[i + 1] = g / a; px[i + 2] = b / a; }
  px[i + 3] = Math.round(255 * a / n);
}

// PNG: signature, IHDR, one IDAT of filter-0 scanlines, IEND
const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let j = 0; j < 8; j++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = buf => { let c = 0xffffffff; for (const v of buf) c = crcTable[(c ^ v) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
const chunk = (type, data) => {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]), c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
  return Buffer.concat([len, td, c]);
};
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(S, 0); ihdr.writeUInt32BE(S, 4); ihdr[8] = 8; ihdr[9] = 6;
const raw = Buffer.alloc((S * 4 + 1) * S);
for (let y = 0; y < S; y++) px.copy(raw, y * (S * 4 + 1) + 1, y * S * 4, (y + 1) * S * 4);
const png = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
fs.writeFileSync(path.join(__dirname, 'icon.png'), png);
console.log('wrote desktop/icon.png', png.length, 'bytes');
