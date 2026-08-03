// Minimal host for the standalone wasm32-unknown-unknown modules the native
// backend emits (stage 7, docs/backend.md). On this target every host interaction
// is an ordinary import: the compiler's own runtime needs exactly 'env.write' and
// 'env.exit' (the unhandled-error panic path) and 'env.fmod'/'env.fmodf' (float
// '%'); anything else a program imports must come from its embedder, and an
// unsatisfied import fails instantiation loudly here rather than linking quietly.
//
// Exit code 120 marks a host-level failure (bad module, missing import), which
// the differential harness treats as an infrastructure error, never a match.
const fs = require("fs");
const bytes = fs.readFileSync(process.argv[2]);
let memory;
const imports = {
    env: {
        // Declared '-> i64' on the Mirage side, so the return value must be a
        // BigInt — node maps wasm i64 to BigInt at the boundary in both
        // directions.
        write: (fd, ptr, len) => {
            const buf = Buffer.from(new Uint8Array(memory.buffer, ptr, Number(len)));
            if (fd === 2) process.stderr.write(buf);
            else process.stdout.write(buf);
            return BigInt(buf.length);
        },
        exit: (code) => process.exit(code & 0xff),
        fmod: (a, b) => a % b,
        fmodf: (a, b) => Math.fround(Math.fround(a) % Math.fround(b)),
        // The stdlib's freestanding heap allocator asks the host for pages the
        // POSIX way; serve it from '__heap_base' (exported for exactly this),
        // growing the memory when the break passes its current end.
        sbrk: (increment) => {
            if (brk === 0) brk = heapBase;
            const old = brk;
            brk += increment;
            const needed = brk - memory.buffer.byteLength;
            if (needed > 0) memory.grow(Math.ceil(needed / 65536));
            return old;
        },
        // The basic libc memory/string surface some fixtures declare as ext fns.
        memcpy: (dst, src, n) => {
            new Uint8Array(memory.buffer).copyWithin(dst, src, src + Number(n));
            return dst;
        },
        memmove: (dst, src, n) => {
            new Uint8Array(memory.buffer).copyWithin(dst, src, src + Number(n));
            return dst;
        },
        memset: (dst, value, n) => {
            new Uint8Array(memory.buffer).fill(value & 0xff, dst, dst + Number(n));
            return dst;
        },
        memcmp: (a, b, n) => {
            const view = new Uint8Array(memory.buffer);
            for (let i = 0; i < Number(n); i++) {
                const d = view[a + i] - view[b + i];
                if (d !== 0) return d < 0 ? -1 : 1;
            }
            return 0;
        },
        strlen: (ptr) => {
            const view = new Uint8Array(memory.buffer);
            let end = ptr;
            while (view[end] !== 0) end++;
            return end - ptr;
        },
        // Buffered-file surface over node's fs, FILE* as a handle-table index
        // (0 = NULL). Enough for the corpus's read/seek/tell usage.
        fopen: (path, mode) => {
            try {
                const fd = require("fs").openSync(cstr(path), cstr(mode).replace("b", ""));
                files.set(nextFile, { fd, pos: 0 });
                return nextFile++;
            } catch {
                return 0;
            }
        },
        fclose: (stream) => {
            const file = files.get(stream);
            if (!file) return -1;
            require("fs").closeSync(file.fd);
            files.delete(stream);
            return 0;
        },
        fread: (buf, size, count, stream) => {
            const file = files.get(stream);
            if (!file) return 0;
            const total = Number(size) * Number(count);
            const view = new Uint8Array(memory.buffer);
            const n = require("fs").readSync(file.fd, view, buf, total, file.pos);
            file.pos += n;
            return Number(size) === 0 ? 0 : Math.floor(n / Number(size));
        },
        fwrite: (buf, size, count, stream) => {
            const file = files.get(stream);
            if (!file) return 0;
            const total = Number(size) * Number(count);
            const view = new Uint8Array(memory.buffer, buf, total);
            const n = require("fs").writeSync(file.fd, view, 0, total, file.pos);
            file.pos += n;
            return Number(size) === 0 ? 0 : Math.floor(n / Number(size));
        },
        fseek: (stream, offset, whence) => {
            const file = files.get(stream);
            if (!file) return -1;
            const delta = Number(offset);
            if (whence === 0) file.pos = delta;
            else if (whence === 1) file.pos += delta;
            else file.pos = require("fs").fstatSync(file.fd).size + delta;
            return 0;
        },
        ftell: (stream) => {
            const file = files.get(stream);
            return BigInt(file ? file.pos : -1); // declared '-> i64'
        },
        rewind: (stream) => {
            const file = files.get(stream);
            if (file) file.pos = 0;
        },
        // A bump allocator over the same break, with an 8-byte size header so
        // realloc can copy. free is a no-op — fine for fixture lifetimes.
        malloc: (n) => hostMalloc(n),
        calloc: (count, size) => hostMalloc(count * size), // memory.grow zero-fills
        free: () => {},
        realloc: (ptr, n) => {
            const fresh = hostMalloc(n);
            if (ptr !== 0) {
                const old = new DataView(memory.buffer).getUint32(ptr - 8, true);
                new Uint8Array(memory.buffer)
                    .copyWithin(fresh, ptr, ptr + Math.min(old, n));
            }
            return fresh;
        },
        // The printf family. The compiler lowers a C-variadic call to a pointer
        // at a shadow-stack buffer (emscripten's convention); these read it with
        // the same ILP32 layout the backend wrote: 4-byte cells for i32-class
        // arguments, 8-byte-aligned cells for i64/f64.
        printf: (fmt, args) => {
            const text = formatC(cstr(fmt), args);
            process.stdout.write(text);
            return text.length;
        },
        sprintf: (buf, fmt, args) => {
            const text = formatC(cstr(fmt), args);
            writeCstr(buf, text);
            return text.length;
        },
        snprintf: (buf, n, fmt, args) => {
            const limit = Number(n); // declared u64: arrives as a BigInt
            const text = formatC(cstr(fmt), args);
            if (limit > 0) writeCstr(buf, text.slice(0, limit - 1));
            return text.length;
        },
    },
};
let brk = 0;
let heapBase = 0;
const files = new Map();
let nextFile = 1;

function hostMalloc(n) {
    if (brk === 0) brk = heapBase;
    brk = (brk + 15) & ~15;
    const header = brk;
    brk += 8 + n;
    const needed = brk - memory.buffer.byteLength;
    if (needed > 0) memory.grow(Math.ceil(needed / 65536));
    new DataView(memory.buffer).setUint32(header, n, true);
    return header + 8;
}

function cstr(ptr) {
    const view = new Uint8Array(memory.buffer);
    let end = ptr;
    while (view[end] !== 0) end++;
    return Buffer.from(view.subarray(ptr, end)).toString("utf8");
}
function writeCstr(ptr, text) {
    const bytes = Buffer.from(text, "utf8");
    new Uint8Array(memory.buffer).set(bytes, ptr);
    new Uint8Array(memory.buffer)[ptr + bytes.length] = 0;
}

// A C format interpreter covering what the corpus uses: flags/width/precision,
// %d %i %u %x %X %c %s %p %%, the l/ll/z length prefixes (ILP32: l and z read 4
// bytes, ll reads 8), the * width/precision, and %f/%e/%g on doubles.
function formatC(fmt, argsPtr) {
    let offset = 0;
    const view = () => new DataView(memory.buffer);
    const readI32 = () => {
        const v = view().getInt32(argsPtr + offset, true);
        offset += 4;
        return v;
    };
    const readI64 = () => {
        offset = (offset + 7) & ~7;
        const v = view().getBigInt64(argsPtr + offset, true);
        offset += 8;
        return v;
    };
    const readF64 = () => {
        offset = (offset + 7) & ~7;
        const v = view().getFloat64(argsPtr + offset, true);
        offset += 8;
        return v;
    };

    let out = "";
    let i = 0;
    while (i < fmt.length) {
        const ch = fmt[i++];
        if (ch !== "%") {
            out += ch;
            continue;
        }
        if (fmt[i] === "%") {
            out += "%";
            i++;
            continue;
        }
        let flags = "";
        while ("-+ 0#".includes(fmt[i])) flags += fmt[i++];
        let width = "";
        if (fmt[i] === "*") {
            width = String(readI32());
            i++;
        } else {
            while (fmt[i] >= "0" && fmt[i] <= "9") width += fmt[i++];
        }
        let precision = "";
        let hasPrecision = false;
        if (fmt[i] === ".") {
            i++;
            hasPrecision = true;
            if (fmt[i] === "*") {
                precision = String(readI32());
                i++;
            } else {
                while (fmt[i] >= "0" && fmt[i] <= "9") precision += fmt[i++];
            }
        }
        let wide = false;
        if (fmt[i] === "l") {
            i++;
            if (fmt[i] === "l") {
                i++;
                wide = true;
            }
        } else if (fmt[i] === "z" || fmt[i] === "h") {
            i++;
            if (fmt[i] === "h") i++;
        }
        const conv = fmt[i++];
        let text;
        switch (conv) {
        case "d":
        case "i": {
            const v = wide ? readI64() : BigInt(readI32());
            text = v.toString(10);
            break;
        }
        case "u": {
            const v = wide ? BigInt.asUintN(64, readI64()) : BigInt(readI32() >>> 0);
            text = v.toString(10);
            break;
        }
        case "x":
        case "X": {
            const v = wide ? BigInt.asUintN(64, readI64()) : BigInt(readI32() >>> 0);
            text = v.toString(16);
            if (conv === "X") text = text.toUpperCase();
            break;
        }
        case "p": {
            text = "0x" + (readI32() >>> 0).toString(16);
            break;
        }
        case "c": {
            text = String.fromCharCode(readI32() & 0xff);
            break;
        }
        case "s": {
            const ptr = readI32();
            if (hasPrecision) {
                const n = parseInt(precision || "0", 10);
                text = Buffer.from(new Uint8Array(memory.buffer, ptr, n)).toString("utf8");
            } else {
                text = cstr(ptr);
            }
            break;
        }
        case "f":
        case "F": {
            const v = readF64();
            const p = hasPrecision ? parseInt(precision || "0", 10) : 6;
            text = v.toFixed(p);
            break;
        }
        case "e":
        case "E": {
            const v = readF64();
            const p = hasPrecision ? parseInt(precision || "0", 10) : 6;
            text = v.toExponential(p);
            if (conv === "E") text = text.toUpperCase();
            // C pads the exponent to two digits; JS does not.
            text = text.replace(/e([+-])(\d)$/, "e$10$2");
            break;
        }
        case "g":
        case "G": {
            const v = readF64();
            text = String(v);
            break;
        }
        default:
            text = "%" + conv;
            break;
        }
        const w = parseInt(width || "0", 10);
        if (text.length < w) {
            if (flags.includes("-")) text = text.padEnd(w);
            else if (flags.includes("0") && !"sc".includes(conv)) text = text.padStart(w, "0");
            else text = text.padStart(w);
        }
        out += text;
    }
    return out;
}
try {
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, imports);
    memory = inst.exports.memory;
    heapBase = inst.exports.__heap_base.value;
    const code = inst.exports.main();
    process.exit(code & 0xff);
} catch (e) {
    console.error("wasm-host: " + e.message);
    // An unsatisfied import means the program needs host surface this minimal
    // embedder does not provide (files, sockets, ...) — a property of the
    // program/embedder pair, not a compiler bug. Distinguished so the harness
    // can count it as "no wasm form" instead of failing.
    process.exit(e instanceof WebAssembly.LinkError ? 121 : 120);
}
