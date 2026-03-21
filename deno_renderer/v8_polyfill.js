// Polyfill TextEncoder/TextDecoder for bare V8 (deno_core doesn't provide them).
// Must execute before protobuf-es module init.
//
// Uses function constructors (not class syntax) for maximum V8 compatibility.

(function() {
  if (typeof globalThis.TextEncoder === "undefined" ||
      typeof globalThis.TextEncoder !== "function") {
    function TextEncoderPolyfill() {}
    TextEncoderPolyfill.prototype.encode = function(str) {
      if (typeof str !== "string") str = String(str);
      var buf = new Uint8Array(str.length * 3);
      var pos = 0;
      for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c < 0x80) {
          buf[pos++] = c;
        } else if (c < 0x800) {
          buf[pos++] = 0xc0 | (c >> 6);
          buf[pos++] = 0x80 | (c & 0x3f);
        } else if (c < 0xd800 || c >= 0xe000) {
          buf[pos++] = 0xe0 | (c >> 12);
          buf[pos++] = 0x80 | ((c >> 6) & 0x3f);
          buf[pos++] = 0x80 | (c & 0x3f);
        } else {
          // surrogate pair
          i++;
          c = 0x10000 + (((c & 0x3ff) << 10) | (str.charCodeAt(i) & 0x3ff));
          buf[pos++] = 0xf0 | (c >> 18);
          buf[pos++] = 0x80 | ((c >> 12) & 0x3f);
          buf[pos++] = 0x80 | ((c >> 6) & 0x3f);
          buf[pos++] = 0x80 | (c & 0x3f);
        }
      }
      return buf.subarray(0, pos);
    };
    TextEncoderPolyfill.prototype.encoding = "utf-8";
    globalThis.TextEncoder = TextEncoderPolyfill;
  }

  if (typeof globalThis.TextDecoder === "undefined" ||
      typeof globalThis.TextDecoder !== "function") {
    function TextDecoderPolyfill() {
      this.encoding = "utf-8";
    }
    TextDecoderPolyfill.prototype.decode = function(buf) {
      if (!buf || buf.byteLength === 0) return "";
      var bytes = new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength || buf.length);
      var str = "";
      for (var i = 0; i < bytes.length;) {
        var c = bytes[i];
        if (c < 0x80) {
          str += String.fromCharCode(c);
          i++;
        } else if (c < 0xe0) {
          str += String.fromCharCode(((c & 0x1f) << 6) | (bytes[i+1] & 0x3f));
          i += 2;
        } else if (c < 0xf0) {
          str += String.fromCharCode(((c & 0x0f) << 12) | ((bytes[i+1] & 0x3f) << 6) | (bytes[i+2] & 0x3f));
          i += 3;
        } else {
          var cp = ((c & 0x07) << 18) | ((bytes[i+1] & 0x3f) << 12) | ((bytes[i+2] & 0x3f) << 6) | (bytes[i+3] & 0x3f);
          cp -= 0x10000;
          str += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
          i += 4;
        }
      }
      return str;
    };
    globalThis.TextDecoder = TextDecoderPolyfill;
  }

  // Diagnostic
  try {
    var te = new globalThis.TextEncoder();
    var td = new globalThis.TextDecoder();
    var encoded = te.encode("test");
    var decoded = td.decode(encoded);
    if (decoded === "test") {
      // polyfill works
    } else {
      throw new Error("roundtrip failed: got " + decoded);
    }
  } catch(e) {
    // Log but don't crash — proto bundle will fail with a clear error
    if (typeof Deno !== "undefined" && Deno.core && Deno.core.ops && Deno.core.ops.op_log) {
      Deno.core.ops.op_log("TextEncoder polyfill self-test failed: " + e);
    }
  }
})();
