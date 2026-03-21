// Polyfill TextEncoder/TextDecoder for bare V8 (deno_core doesn't provide them).
// Must execute before protobuf-es module init.
if (typeof globalThis.TextEncoder === "undefined") {
  globalThis.TextEncoder = class TextEncoder {
    encode(str) {
      const buf = new Uint8Array(str.length * 3);
      let pos = 0;
      for (let i = 0; i < str.length; i++) {
        let c = str.charCodeAt(i);
        if (c < 0x80) {
          buf[pos++] = c;
        } else if (c < 0x800) {
          buf[pos++] = 0xc0 | (c >> 6);
          buf[pos++] = 0x80 | (c & 0x3f);
        } else {
          buf[pos++] = 0xe0 | (c >> 12);
          buf[pos++] = 0x80 | ((c >> 6) & 0x3f);
          buf[pos++] = 0x80 | (c & 0x3f);
        }
      }
      return buf.subarray(0, pos);
    }
  };
}
if (typeof globalThis.TextDecoder === "undefined") {
  globalThis.TextDecoder = class TextDecoder {
    decode(buf) {
      if (!buf) return "";
      const bytes = new Uint8Array(buf);
      let str = "";
      for (let i = 0; i < bytes.length;) {
        let c = bytes[i];
        if (c < 0x80) { str += String.fromCharCode(c); i++; }
        else if (c < 0xe0) { str += String.fromCharCode(((c & 0x1f) << 6) | (bytes[i+1] & 0x3f)); i += 2; }
        else { str += String.fromCharCode(((c & 0x0f) << 12) | ((bytes[i+1] & 0x3f) << 6) | (bytes[i+2] & 0x3f)); i += 3; }
      }
      return str;
    }
  };
}
