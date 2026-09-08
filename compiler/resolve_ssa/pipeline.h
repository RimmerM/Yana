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
    /*
     * Constructing one *is* choosing this stage, so the setting is set here rather than by the
     * caller.
     *
     * It is read below this - the stage decides how a program is resolved, and a caller that had to
     * remember the assignment would be a second statement of which stage is running. It used to
     * decide which *library* was read as well (`prelude/` here, `lib/` there); both stages read
     * `lib/` since 2026-09-07 and `LibrarySource::directory` no longer asks.
     */
    Pipeline(Context& context): context(context) {
        context.settings.resolver = ResolverStage::Ownership;
    }

    Context& context;

    Ptr<mir::Program> program;
    Converted converted;

    // Parse to verified MIR. False if the program was rejected or the IR failed verification;
    // `program` is still set either way, so a driver can print what it got. `provider` is where a
    // module an `import` names comes from when it is not the standard library's, and `testRoots` is
    // the modules that declare a `@test` and that nothing imports - see `mir::resolveProgram`.
    bool resolve(ast::ModuleGroup& root, ast::ModuleProvider* provider = nullptr,
                 Buffer<ast::ModuleGroup*> testRoots = {});
    bool resolve(ast::Module& root, ast::ModuleProvider* provider = nullptr);

    // Verified MIR to verified SSA.
    bool convert();

    /*
     * The optimizer over that SSA - `opt/`, and `settings.optimizeIr` decides whether any pass runs.
     *
     * Between `convert` and `lower`, and **after `releaseMir`**: the optimizer reads declarations,
     * which live in the decl region and survive, and never MIR. Which is also why the SSA dump is
     * below this rather than above it - `-mode ssa` prints what the backend gets, exactly as
     * `-mode ir` does for the shipping stage, and `-no-ir-opt` is how the form before it is asked
     * for.
     *
     * False if a pass produced IR the verifier rejects, which has been reported by then. A program
     * that failed above this answers true and does nothing, on the same terms as `lower`.
     */
    bool optimize();

    // Verified SSA to the shared lower IR. Null if anything above failed.
    Ptr<LowerModule> lower();

    // Releases the MIR region. After this the MIR dump is no longer available, which is the point:
    // §8.8's rule is that nothing below may hold an MIR handle, and a released region is what makes
    // an accidental one fail loudly rather than read stale bytes.
    void releaseMir();
};

} // namespace ssa
