// One program per message, each in an emu of its own (emu-web.js, the
// Emscripten module of build-wasm's emu-web): the ELF in its memory file
// system, main run as `emu [args] /prog.elf`.
importScripts("emu-web.js");
onmessage = async (e) => {
    const {elf, args} = e.data;
    const out = [];
    const t0 = performance.now();
    const emu = await createEmu({print: (s) => out.push(s), printErr: (s) => out.push("stderr: " + s)});
    const t1 = performance.now();
    emu.FS.writeFile("/prog.elf", new Uint8Array(elf));
    let rc = null;
    try {
        rc = emu.callMain([...args, "/prog.elf"]);
    } catch (error) {
        out.push("exception: " + error);
    }
    postMessage({rc, out, init_ms: t1 - t0, run_ms: performance.now() - t1});
};
