// Entry point for esbuild bundling — exposes protobuf types on globalThis.proto
// Note: v8_polyfill.js is injected via esbuild --banner before this code runs.

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
