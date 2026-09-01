#pragma once

#include "convert.h"
#include "../resolve_mir/resolve.h"
#include "../lower/lower.h"

namespace ssa {

/*
 * The whole new stage, behind one type - Part 11.2's `-resolver ownership`.
 *
 * A driver holds one of these rather than three arenas and a call order, because the order is not a
 * caller's business: MIR is verified before it is converted, SSA is verified before it is lowered,
 * and the MIR region is released once nothing needs it. A caller that had to arrange that would be a
 * second place the phase boundary is stated.
 *
 * The MIR region is *not* released automatically, and that is deliberate: `-mode mir` and the
 * `.mir.expect` golden print MIR and stop there, so the release is a call the driver makes once it
 * knows it is past that point rather than something the conversion does behind it. The place to make
 * it is immediately after `convert`, before `lower` - nothing below conversion may hold an MIR
 * handle, so holding the region any longer is a peak of three IRs where two would do.
 */
struct Pipeline {
    Pipeline(Context& context): context(context) {}

    Context& context;

    Ptr<mir::Program> program;
    Converted converted;

    // Parse to verified MIR. False if the program was rejected or the IR failed verification;
    // `program` is still set either way, so a driver can print what it got. `provider` is where a
    // module an `import` names comes from when it is not the standard library's - see
    // `mir::resolveProgram`.
    bool resolve(ast::ModuleGroup& root, ast::ModuleProvider* provider = nullptr);
    bool resolve(ast::Module& root, ast::ModuleProvider* provider = nullptr);

    // Verified MIR to verified SSA.
    bool convert();

    // Verified SSA to the shared lower IR. Null if anything above failed.
    Ptr<LowerModule> lower();

    // Releases the MIR region. After this the MIR dump is no longer available, which is the point:
    // §8.8's rule is that nothing below may hold an MIR handle, and a released region is what makes
    // an accidental one fail loudly rather than read stale bytes.
    void releaseMir();
};

} // namespace ssa
