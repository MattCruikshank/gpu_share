// Entry point for esbuild bundling — exposes protobuf types on globalThis.proto

// Polyfill TextEncoder/TextDecoder for bare V8 (deno_core doesn't provide them)
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

import { fromBinary } from "@bufbuild/protobuf";
import { InputEventSchema } from "./gen/gpu_share_pb.js";

globalThis.proto = {
  fromBinary,
  InputEventSchema,

  /**
   * Decode length-prefixed protobuf events from op_gpu_poll_events().
   * Format: [u32le length][protobuf bytes][u32le length][protobuf bytes]...
   * Returns an array of decoded InputEvent objects.
   */
  decodeEvents(buf) {
    const events = [];
    const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    let offset = 0;
    while (offset + 4 <= buf.length) {
      const len = view.getUint32(offset, true); // little-endian
      offset += 4;
      if (offset + len > buf.length) break;
      const slice = buf.subarray(offset, offset + len);
      events.push(fromBinary(InputEventSchema, slice));
      offset += len;
    }
    return events;
  },
};
