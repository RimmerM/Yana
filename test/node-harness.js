// One Node process for the whole fixture run, instead of one per compiled program.
//
// Starting Node is around eleven milliseconds and the resolve suite does it a hundred and seventy
// times - once for the optimized build of every fixture with a `.js.expect`, and once more for the
// unoptimized build it is compared against. That is most of the wall time of the JavaScript half and
// none of it is spent on the property being asserted.
//
// What must not be shared is the *program*. These fixtures exist to say what a program means on this
// target, and a program that means it only because of what the previous fixture left in the global
// object means nothing - so every script is evaluated with `vm.runInNewContext`, which gives it a
// fresh global and fresh intrinsics. What is shared is the process, which no fixture can observe:
// the emitted code is self-contained, referring to nothing outside itself but `console.log`.
//
// The protocol, over stdin and stdout, is a decimal byte count on a line of its own followed by that
// many bytes:
//
//     -> 1234\n<1234 bytes of script>
//     <- OK 3\n42\n              (what the script printed, the last line being main's answer)
//     <- ERR 118\n<the stack>    (the script threw; the driver reports it as a failure)
//
// A blank request line, or a closed stdin, ends the run.

const fs = require('fs');
const vm = require('vm');
const util = require('util');

function readExactly(length) {
    const out = Buffer.alloc(length);
    let got = 0;

    while(got < length) {
        const read = fs.readSync(0, out, got, length - got, null);
        if(read === 0) return null;
        got += read;
    }

    return out;
}

// Byte at a time, because the length is what says where the script starts and reading past it would
// consume the script's first bytes. Headers are a handful of bytes and there is one per fixture.
function readLine() {
    const one = Buffer.alloc(1);
    let line = '';

    for(;;) {
        const read = fs.readSync(0, one, 0, 1, null);
        if(read === 0) return line.length ? line : null;
        if(one[0] === 0x0a) return line;

        line += String.fromCharCode(one[0]);
    }
}

function respond(status, text) {
    const payload = Buffer.from(text, 'utf8');
    fs.writeSync(1, status + ' ' + payload.length + '\n');

    let wrote = 0;
    while(wrote < payload.length) wrote += fs.writeSync(1, payload, wrote, payload.length - wrote);
}

for(;;) {
    let header;

    try {
        header = readLine();
    } catch(e) {
        // EOF on some platforms arrives as a throw rather than a zero-length read.
        break;
    }

    if(header === null || header === '') break;

    const length = parseInt(header, 10);
    if(!(length >= 0)) break;

    const script = readExactly(length);
    if(script === null) break;

    let output = '';

    // The real `console.log`'s formatting, so that a fixture which prints is asserted against what
    // it would have printed under a Node of its own rather than against this shim's idea of it.
    const sandbox = {
        console: {
            log: (...args) => { output += util.format(...args) + '\n'; },
            error: (...args) => { output += util.format(...args) + '\n'; },
        },
    };

    try {
        // The completion value is the status the program answered: an emitted file ends with a call
        // of its own entry, so the last thing the script evaluates is that call. Written as the
        // final line, which is where the driver reads it from - anything before it is what the
        // fixture itself printed.
        //
        // `String()` rather than the value, because an `I64` is a BigInt here and would otherwise
        // print as `37n`. A program whose entry answers nothing completes as `undefined` and is
        // reported as zero, which is what the native wrapper says about the same program.
        const status = vm.runInNewContext(script.toString('utf8'), sandbox, { filename: 'fixture.js' });
        output += String(status === undefined ? 0 : status) + '\n';

        respond('OK', output);
    } catch(e) {
        respond('ERR', output + (e && e.stack ? e.stack : String(e)) + '\n');
    }
}
