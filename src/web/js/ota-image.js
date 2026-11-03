"use strict";

const TAG_PREFIX = "OPENBREWBYWEIGHT_FW_TAG_V2|",
  TAG_END = "|END",
  TAG_BODY_MAX = 256;
const IMAGE_PREFIX_BYTES = 288,
  IMAGE_HASH_BYTES = 32,
  IMAGE_BLOCK_BYTES = 4096;
const SHA256_K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

// Whole 64-byte blocks are hashed in place and the message schedule is reused,
// so a firmware image is not copied byte by byte; fork() continues a digest.
function hash256(from) {
  let state = from
      ? from.state.slice()
      : [
          1779033703, 3144134277, 1013904242, 2773480762, 1359893119, 2600822924, 528734635,
          1541459225,
        ],
    used = from ? from.used : 0,
    total = from ? from.total : 0;
  const buffer = from ? from.buffer.slice() : new Uint8Array(64),
    words = new Uint32Array(64),
    rr = (value, bits) => (value >>> bits) | (value << (32 - bits));
  function block(input, at = 0) {
    for (let i = 0; i < 16; i++)
      words[i] =
        (input[at + i * 4] << 24) |
        (input[at + i * 4 + 1] << 16) |
        (input[at + i * 4 + 2] << 8) |
        input[at + i * 4 + 3];
    for (let i = 16; i < 64; i++) {
      const x = words[i - 15],
        y = words[i - 2];
      words[i] =
        (words[i - 16] +
          (rr(x, 7) ^ rr(x, 18) ^ (x >>> 3)) +
          words[i - 7] +
          (rr(y, 17) ^ rr(y, 19) ^ (y >>> 10))) >>>
        0;
    }
    let [a, b, c, d, e, f, g, h] = state;
    for (let i = 0; i < 64; i++) {
      const sum1 = rr(e, 6) ^ rr(e, 11) ^ rr(e, 25),
        choose = (e & f) ^ (~e & g),
        temp1 = (h + sum1 + choose + SHA256_K[i] + words[i]) >>> 0,
        sum0 = rr(a, 2) ^ rr(a, 13) ^ rr(a, 22),
        majority = (a & b) ^ (a & c) ^ (b & c),
        temp2 = (sum0 + majority) >>> 0;
      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }
    state = [
      (state[0] + a) >>> 0,
      (state[1] + b) >>> 0,
      (state[2] + c) >>> 0,
      (state[3] + d) >>> 0,
      (state[4] + e) >>> 0,
      (state[5] + f) >>> 0,
      (state[6] + g) >>> 0,
      (state[7] + h) >>> 0,
    ];
  }
  return {
    add(bytes) {
      let i = 0;
      total += bytes.length;
      if (used) {
        while (used < 64 && i < bytes.length) buffer[used++] = bytes[i++];
        if (used < 64) return;
        block(buffer);
        used = 0;
      }
      for (; i + 64 <= bytes.length; i += 64) block(bytes, i);
      while (i < bytes.length) buffer[used++] = bytes[i++];
    },
    fork() {
      return hash256({ state, buffer, used, total });
    },
    end() {
      const bits = total * 8;
      buffer[used++] = 128;
      if (used > 56) {
        while (used < 64) buffer[used++] = 0;
        block(buffer);
        used = 0;
      }
      while (used < 56) buffer[used++] = 0;
      for (let i = 7; i >= 0; i--) buffer[used++] = Math.floor(bits / 2 ** (i * 8)) & 255;
      block(buffer);
      return state.map((value) => value.toString(16).padStart(8, "0")).join("");
    },
  };
}

function tagFields(body) {
  if (body.includes("\0")) return null;
  const f = { arch: "", hw: "", machine: "", ver: "", packed: "" },
    seen = new Set();
  for (const part of body.split("|")) {
    const at = part.indexOf("=");
    if (at <= 0) continue;
    const key = part.slice(0, at),
      value = part.slice(at + 1);
    if (!Object.prototype.hasOwnProperty.call(f, key)) continue;
    if (seen.has(key)) return null;
    seen.add(key);
    if (
      (key === "arch" && (!/^[a-z0-9]{1,15}$/.test(value) || value === "unknown")) ||
      ((key === "hw" || key === "machine") && !/^[a-z0-9][a-z0-9-]{0,62}$/.test(value)) ||
      (key === "ver" && !/^[A-Za-z0-9.+_-]{1,47}$/.test(value)) ||
      (key === "packed" && !/^\d{1,10}$/.test(value))
    )
      return null;
    f[key] = value;
  }
  if (!f.arch || !f.hw || !f.machine || !f.ver || !f.packed) return null;
  const packed = Number(f.packed);
  return Number.isInteger(packed) && packed >= 0 && packed <= 4294967295
    ? { arch: f.arch, hardware: f.hw, machine: f.machine, version: f.ver, packed }
    : null;
}

function imageTagScanner() {
  let tail = "",
    consumed = 0,
    found = null;
  const decoder = new TextDecoder("latin1"),
    keep = TAG_PREFIX.length + TAG_BODY_MAX;
  return {
    add(bytes) {
      if (found) {
        consumed += bytes.length;
        return;
      }
      const text = decoder.decode(bytes),
        joined = tail + text,
        base = consumed - tail.length;
      let from = 0;
      for (;;) {
        const at = joined.indexOf(TAG_PREFIX, from);
        if (at < 0) break;
        const bodyAt = at + TAG_PREFIX.length,
          end = joined.indexOf(TAG_END, bodyAt);
        if (end >= bodyAt && end - bodyAt + TAG_END.length <= TAG_BODY_MAX) {
          const tag = tagFields(joined.slice(bodyAt, end));
          if (tag) {
            found = { ...tag, tagOffset: base + at };
            break;
          }
        }
        from = at + 1;
      }
      tail = joined.slice(-keep);
      consumed += bytes.length;
    },
    end() {
      return found;
    },
  };
}

function hex(bytes) {
  return Array.from(bytes, (x) => x.toString(16).padStart(2, "0")).join("");
}

function imageHeader(bytes) {
  if (bytes[0] !== 0xe9) throw new Error(__WEBUI_TEXT__("ota.this_file_does_not_have_an_esp"));
  if (bytes[12] !== 9 || bytes[13] !== 0)
    throw new Error(__WEBUI_TEXT__("ota.this_firmware_was_not_built_for_an"));
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(32, true) !== 0xabcd5432)
    throw new Error(__WEBUI_TEXT__("ota.this_firmware_has_an_invalid_application_descriptor"));
  let project = "";
  for (let i = 80; i < 112 && bytes[i]; i++) project += String.fromCharCode(bytes[i]);
  if (project !== "openbrewbyweight")
    throw new Error(__WEBUI_TEXT__("ota.this_firmware_does_not_belong_to_the"));
  if (bytes[23] !== 1)
    throw new Error(__WEBUI_TEXT__("ota.this_firmware_has_no_appended_image_checksum"));
  return project;
}

export async function otaFileIdentity(file) {
  if (!file || file.size < IMAGE_PREFIX_BYTES + IMAGE_HASH_BYTES)
    throw new Error(__WEBUI_TEXT__("ota.this_file_is_too_short_to_be_2"));
  const header = new Uint8Array(await file.slice(0, IMAGE_PREFIX_BYTES).arrayBuffer()),
    projectName = imageHeader(header),
    imageHash = hash256(),
    scanner = imageTagScanner(),
    step = 65536,
    bodyBytes = file.size - IMAGE_HASH_BYTES,
    blocks = [];
  for (let at = 0; at < file.size; at += step) {
    const end = Math.min(file.size, at + step),
      bytes = new Uint8Array(await file.slice(at, end).arrayBuffer());
    scanner.add(bytes);
    for (let from = 0; from < bytes.length; from += IMAGE_BLOCK_BYTES) {
      const block = hash256();
      block.add(bytes.subarray(from, Math.min(bytes.length, from + IMAGE_BLOCK_BYTES)));
      blocks.push(block.end());
    }
    if (at < bodyBytes) imageHash.add(bytes.subarray(0, Math.min(bytes.length, bodyBytes - at)));
  }
  const tag = scanner.end();
  if (!tag) throw new Error(__WEBUI_TEXT__("ota.this_bin_does_not_contain_a_valid"));
  const expected = new Uint8Array(await file.slice(bodyBytes, file.size).arrayBuffer()),
    // The whole-file digest continues the body digest over the trailing hash.
    hash = imageHash.fork();
  hash.add(expected);
  if (imageHash.end() !== hex(expected))
    throw new Error(__WEBUI_TEXT__("ota.this_firmware_image_is_truncated_or_corrupt"));
  const verifyRange = async (offset, end) => {
    if (offset < 0 || offset % IMAGE_BLOCK_BYTES || end > file.size || offset >= end) return false;
    try {
      const bytes = new Uint8Array(await file.slice(offset, end).arrayBuffer());
      for (
        let at = offset, index = (at / IMAGE_BLOCK_BYTES) | 0, used = 0;
        at < end;
        at += IMAGE_BLOCK_BYTES, index++, used += IMAGE_BLOCK_BYTES
      ) {
        const block = hash256();
        block.add(bytes.subarray(used, used + Math.min(end, at + IMAGE_BLOCK_BYTES) - at));
        if (block.end() !== blocks[index]) return false;
      }
      return true;
    } catch {
      return false;
    }
  };
  return {
    size: file.size,
    sha256: hash.end(),
    imageSha256: hex(expected),
    arch: tag.arch,
    hardware: tag.hardware,
    machine: tag.machine,
    version: tag.version,
    packed: tag.packed,
    tagOffset: tag.tagOffset,
    projectName,
    verifyRange,
  };
}
