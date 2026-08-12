/*
 * Describing a type in the words a diagnostic uses.
 *
 * Separate from everything that decides anything: nothing here is asked a question whose answer
 * changes what is compiled, so a change to the wording cannot change a program.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

void describeType(Context& context, GlobalBase base, TypePtr type, StringBuilder& target) {
    if(!type) {
        target << "<none>";
        return;
    }

    switch(base[type]->kind) {
        case Type::Error:
            target << "<error>";
            return;
        case Type::Unit:
            target << "()";
            return;
        case Type::Int: {
            // A refinement says so, and then says what it refines. Without this a diagnostic about
            // `Id` and `U64` reads "cannot convert U64 to U64", which names the problem twice and
            // identifies it not at all.
            auto integer = (IntType*)base[type];
            if(integer->canonical) {
                // appendValue rather than `<<`, which takes a character and would quietly append
                // the one with that code - `@bits(53)` came out as `@bits(5)`.
                target << "@bits(";
                target.appendValue(integer->bits);
                target << ") ";
                describeType(context, base, integer->canonical, target);
                return;
            }

            // Core's Int and Native's I32 have the same shape and different names, so an integer
            // type says which one it is rather than describing its width.
            auto name = integer->name;
            if(name) {
                target << context.findName(name);
                return;
            }

            switch(((IntType*)base[type])->width) {
                case IntType::Bool: target << "Bool"; return;
                case IntType::Int: target << "Int"; return;
                case IntType::Long: target << "Long"; return;
            }
            return;
        }
        case Type::Ptr:
            target << '%';
            describeType(context, base, ((PtrType*)base[type])->to, target);
            return;
        case Type::String:
            // One name on both targets, which is the whole of what making it a primitive buys a
            // diagnostic: nothing a program reads ever says which of the two representations it is.
            target << "String";
            return;
        case Type::Array: {
            // Printed the way it is written, count included, because the count is the whole of what
            // two of these differ in and a diagnostic dropping it would name both `[Int *]`. The
            // count goes through describeType rather than being a number, so `[a *n]` prints its
            // variable's name - Implementation-Const-Generics.md §2.1.
            auto array = (ArrayType*)base[type];
            target << '[';
            describeType(context, base, array->content, target);
            target << " *";
            describeType(context, base, array->count, target);
            target << ']';
            return;
        }
        case Type::Const:
            // A count prints as the number it is, with no mention of what it is a number *of*: the
            // type in its interning key exists to keep two parameters apart and is not something a
            // diagnostic naming `[Int *4]` should be saying.
            target.appendValue(((ConstType*)base[type])->value);
            return;
        case Type::Vector: {
            /*
             * The natural form where the count is the target's natural one, and the explicit form
             * otherwise - Implementation-Vector.md §1.4.
             *
             * This matters more than it sounds. `Vec(Float)` is four lanes under SSE2 and eight
             * under AVX2, so printing the resolved count would make every diagnostic and every
             * `.expect` file that mentions a vector target-specific - and the fixture suite would
             * have to be regenerated per feature level rather than compared across them. What is
             * *lost* is nothing a reader needs: the natural form printed back is the source they
             * wrote, and a count that is not the natural one is exactly the case where they wrote
             * one and want to see it.
             *
             * A mask prints as `Mask(a)` with no count at all, since its content is normalized to
             * the unsigned integer of the lane width and the count follows from it.
             */
            auto vector = (VectorType*)base[type];
            auto natural = 0u;

            if(auto stride = laneStride(base, vector->content)) {
                natural = targetVectorBytes(context.settings) / stride;
            }

            target << (vector->isMask ? "Mask(" : "Vec(");
            describeType(context, base, vector->content, target);

            // A null count is the unresolved natural form inside a generic body, which prints as the
            // natural form it is rather than as a count nobody wrote. A *variable* count is always
            // printed, since it is exactly what the programmer wrote and there is no natural form
            // for it to coincide with.
            auto written = writtenCount(base, vector->count);
            if(!vector->isMask && vector->count && (!written || written.unwrap() != natural)) {
                target << ", ";
                describeType(context, base, vector->count, target);
            }

            target << ')';
            return;
        }
        case Type::Borrow:
            // `&T` is how a borrow is written; `&mut T` is a printed form rather than a source one,
            // since what makes a returned borrow mutable is the group it is rooted in rather than
            // anything written on the result - see resolveSignature.
            target << (((BorrowType*)base[type])->mut ? "&mut " : "&");
            describeType(context, base, ((BorrowType*)base[type])->to, target);
            return;
        case Type::Float:
            target << (((FloatType*)base[type])->width == FloatType::Float ? "Float" : "Double");
            return;
        case Type::Gen:
            target << context.findName(((GenType*)base[type])->name);
            return;
        case Type::Literal: {
            // A literal variable only ever appears in a diagnostic about a literal whose type
            // nothing decided, so it says which classes it was waiting to be satisfied by.
            auto literal = (LiteralType*)base[type];
            target << '?';
            target.appendValue(literal->index);

            for(Size i = 0; i < literal->classes.size(); i++) {
                target << (i ? ", " : " (");
                target << context.findName(((TypeClass*)base[literal->classes.get(base, i)])->name);
            }

            if(literal->classes.size()) target << ')';
            return;
        }
        case Type::Tup: {
            auto tuple = (TupType*)base[type];
            target << '{';

            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(i) target << ", ";
                auto field = tuple->fields.get(base, i);

                if(field.name) target << context.findName(field.name) << ": ";

                // Printed even where nothing in the source wrote it, because an automatic
                // indirection is the difference between a type that has a layout and one that does
                // not, and a diagnostic naming the two the same way would be unreadable.
                if(field.boxed) target << "@box ";
                describeType(context, base, field.type, target);
            }

            target << '}';
            return;
        }
        case Type::Fun: {
            // Printed the way it is written, conventions and markers included, because those are
            // what two otherwise identical signatures differ in and a diagnostic that dropped them
            // would name two types the same way.
            auto function = (FunType*)base[type];
            if(function->kind == ast::FunKind::Lens) target << "lens ";
            else if(function->kind == ast::FunKind::Iter) target << "iter ";

            target << '(';
            Size index = 0;

            for(auto arg: function->args.contents(base)) {
                if(index++) target << ", ";
                if(arg.lazy) target << "@lazy ";
                if(arg.returnRoot) target << "return ";
                if(arg.convention == ast::BindType::Ref) target << '&';
                else if(arg.convention == ast::BindType::Sink) target << "->";
                if(arg.name) target << context.findName(arg.name) << ": ";

                describeType(context, base, arg.type, target);
            }

            target << ") -> ";
            describeType(context, base, function->result, target);
            return;
        }
        case Type::Record: {
            auto record = (RecordType*)base[type];

            /*
             * A Repr refinement is printed, and it has to be - Implementation-Containers.md §7.
             *
             * `@inline(4) @capacity(4) [Int]` and `[Int]` are two types with one name, so leaving the
             * refinement off would make a dump ambiguous and, worse, would give the two the same
             * derived-teardown name: teardownGlueName is built out of this text, and two glue
             * functions answering to one symbol is a link-time coin toss.
             */
            // appendValue rather than `<<` for the same reason `@bits` gives above: `<<` takes a
            // character and would append the one with that code.
            if(record->inlineSlots) {
                target << "@inline(";
                target.appendValue(record->inlineSlots);
                target << ") ";
            }

            if(record->capacityBound) {
                target << "@capacity(";
                target.appendValue(record->capacityBound);
                target << ") ";
            }

            target << context.findName(record->name);

            if(record->instanceArgs.isNotEmpty()) {
                target << '(';
                Size index = 0;

                for(auto arg: record->instanceArgs.contents(base)) {
                    if(index++) target << ", ";
                    describeType(context, base, arg, target);
                }

                target << ')';
            }

            return;
        }
        default:
            target << "<unsupported>";
            return;
    }
}

void describeTypes(Context& context, GlobalBase base, Buffer<TypePtr> types, StringBuilder& target) {
    for(Size i = 0; i < types.length; i++) {
        if(i) target << ", ";
        describeType(context, base, types[i], target);
    }
}

String describeType(Context& context, GlobalBase base, TypePtr type) {
    StringBuilder buffer;
    describeType(context, base, type, buffer);
    return buffer.string();
}

StringId builtName(Context& context, StringBuilder& text) {
    return context.addQualifiedName(text.pointer(), text.size(), 1);
}

StringId derivedName(Module& module, StringView prefix, TypePtr type) {
    StringBuilder text;
    text << prefix;
    describeType(module.context, *module.types, type, text);
    return builtName(module.context, text);
}
