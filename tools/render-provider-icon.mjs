import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// Convert the licensed Lucide vector into the DIB resource required by LogonUI.
// Optional argument: path to an existing sharp module (no dependency installation).
const modulePath = process.argv[2];
const { default: sharp } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'sharp');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const provider = path.join(root, 'src', 'provider');
const svg = (await fs.readFile(path.join(provider, 'icons', 'scan-face.svg'), 'utf8'))
  .replace('stroke="currentColor"', 'stroke="#ffffff"')
  .replace('viewBox="0 0 24 24"', 'viewBox="-4 -4 32 32"');
const size = 72;
const { data } = await sharp(Buffer.from(svg)).resize(size, size).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
const headerSize = 54;
const bitmap = Buffer.alloc(headerSize + size * size * 4);
bitmap.write('BM');
bitmap.writeUInt32LE(bitmap.length, 2);
bitmap.writeUInt32LE(headerSize, 10);
bitmap.writeUInt32LE(40, 14);
bitmap.writeInt32LE(size, 18);
bitmap.writeInt32LE(size, 22);
bitmap.writeUInt16LE(1, 26);
bitmap.writeUInt16LE(32, 28);
bitmap.writeUInt32LE(size * size * 4, 34);
for (let y = 0; y < size; y++) {
  for (let x = 0; x < size; x++) {
    const src = (y * size + x) * 4;
    const dst = headerSize + ((size - 1 - y) * size + x) * 4;
    const alpha = data[src + 3];
    // Windows alpha blending consumes premultiplied BGRA.
    bitmap[dst] = Math.round(data[src + 2] * alpha / 255);
    bitmap[dst + 1] = Math.round(data[src + 1] * alpha / 255);
    bitmap[dst + 2] = Math.round(data[src] * alpha / 255);
    bitmap[dst + 3] = alpha;
  }
}
await fs.writeFile(path.join(provider, 'tileimage.bmp'), bitmap);
const previewDir = path.join(root, 'local', 'icon-preview');
await fs.mkdir(previewDir, { recursive: true });
await sharp(Buffer.from(svg)).resize(size, size).png().toFile(path.join(previewDir, 'face-login.png'));
await sharp({ create: { width: 160, height: 96, channels: 4, background: '#333333' } })
  .composite([
    { input: await sharp(Buffer.from(svg)).resize(32, 32).png().toBuffer(), left: 16, top: 32 },
    { input: await sharp(Buffer.from(svg)).resize(72, 72).png().toBuffer(), left: 72, top: 12 },
  ]).png().toFile(path.join(previewDir, 'sizes.png'));
console.log('Generated 72x72 premultiplied BGRA bitmap and 32/72px previews.');
