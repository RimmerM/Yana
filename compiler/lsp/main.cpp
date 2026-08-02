#include "server.h"

#if __WINDOWS__
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

/*
 * yana-lsp - the language server.
 *
 * Links YanaParse, YanaResolve and the project model, and nothing below them: a language server
 * never emits code, and keeping LLVM out of the process is what keeps its start-up in milliseconds
 * and its resident set small. Implementation-Tooling.md §4.
 */

// Everything the compiler prints goes to standard output - diagnostics, `logError`, the module
// list. On a language server that is the protocol stream, and one stray line makes the client
// discard everything after it. So the real standard output is taken away before anything can write
// to it, and what is left pointing at descriptor 1 is standard error.
static bool redirectStandardOutput(int& protocolOut) {
#if __WINDOWS__
    protocolOut = ::_dup(1);
    if(protocolOut < 0) return false;
    ::_dup2(2, 1);
    ::_setmode(0, _O_BINARY);
    ::_setmode(protocolOut, _O_BINARY);
#else
    protocolOut = ::dup(1);
    if(protocolOut < 0) return false;
    ::dup2(2, 1);
#endif

    return true;
}

int main(int argc, const char** argv) {
    int protocolOut = 1;
    if(!redirectStandardOutput(protocolOut)) {
        println("yana-lsp: cannot take over standard output");
        return 1;
    }

    lsp::StdioTransport transport;
    transport.output = protocolOut;

    lsp::Server server(transport);
    return server.run();
}
