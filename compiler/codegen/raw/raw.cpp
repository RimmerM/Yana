#include "raw.h"
#include <File.h>

bool writeRawImage(Context& context, const Byte* bytes, Size size, const String& path) {
    auto opened = File::openFile(path, writeAccess(), File::CreateAlways, File::Executable);
    if(opened.isErr()) {
        context.diagnostics.error("cannot write %@: %@"_v, nullptr, path,
                                  describeError(opened.unwrapErr()));
        return false;
    }

    auto file = opened.moveUnwrapOk();
    for(Size at = 0; at < size;) {
        auto written = file.write({ bytes + at, size - at });
        if(written.isErr()) {
            context.diagnostics.error("cannot write %@: %@"_v, nullptr, path,
                                      describeError(written.unwrapErr()));
            return false;
        }

        if(!written.unwrapOk()) {
            context.diagnostics.error("cannot write %@: the file stopped accepting data after %@ of %@ bytes"_v,
                                      nullptr, path, U64(at), U64(size));
            return false;
        }

        at += written.unwrapOk();
    }

    return true;
}
