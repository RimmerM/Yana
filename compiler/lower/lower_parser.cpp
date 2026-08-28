#include "lower_parser.h"
#include "lower_inst.h"
#include "lower_resolve.h"

LowerParser::LowerParser(Context& context, LowerModule& module, LowerLexer& lexer):
    BasicParser<LowerLexer, LowerToken>(context.diagnostics, lexer, module.name),
    context(context),
    module(module),
    buffer(4 * 1024 * 1024)
{
    mutId = Context::nameHash("mut", 3);
    ptrId = Context::nameHash("Ptr", 3);
    i32Id = Context::nameHash("I32", 3);
    i64Id = Context::nameHash("I64", 3);
    f32Id = Context::nameHash("F32", 3);
    f64Id = Context::nameHash("F64", 3);

    eat();
}

bool LowerParser::parseModule() {
    auto errorCount = context.diagnostics.errorCount();
    auto warningCount = context.diagnostics.warningCount();

    sepBy([&] {
        if(token.type == LowerToken::GlobalID) {
            parseGlobal(false);
        } else if(token.type == LowerToken::LabelID && token.data.id == mutId) {
            // `mut @g = [...]`. The keyword is a bare word and so lexes as a label, which is
            // what a function name is too - so this is decided here, where the token after it
            // settles which of the two it is, rather than by giving the lexer a keyword it
            // would then have to reserve everywhere.
            eat();
            parseGlobal(true);
        } else if(token.type == LowerToken::LabelID) {
            parseDecl();
        } else if(token.type == LowerToken::EndOfStmt) {
            // Skip empty declarations.
            return;
        }

        if(token.type != LowerToken::EndOfStmt && token.type != LowerToken::EndOfFile) {
            // The previous declaration did not parse all tokens.
            // Skip ahead until we are at the root level again, then continue parsing.
            error("expected declaration end"_v);
            eat();
            while(token.startColumn > 0) eat();
        }
    }, LowerToken::EndOfStmt, LowerToken::EndOfFile);

    expect(LowerToken::EndOfFile, "expected file end"_v);

    module.errorCount = context.diagnostics.errorCount() - errorCount;
    module.warningCount = context.diagnostics.warningCount() - warningCount;

    if(module.errorCount > 0) return false;
    return resolveModule();
}

bool LowerParser::resolveModule() {
    LowerResolve resolve(diag, context, module.arena, buffer);
    return resolveLowerModule(resolve, *module.arena, *buffer, module);
}

void LowerParser::parseGlobal(bool mut) {
    auto name = tryMaybe(expectNode(LowerToken::GlobalID, "expected global identifier"_v), return);
    tryMaybe(expect(LowerToken::Equals, "expected '='"_v), return);

    auto g = new (module.arena) LowerGlobal(name.payload.id);
    g->mut = mut;
    g->source = name.node;

    if(token.type == LowerToken::BracketL) {
        Array<U8> contents;

        brackets([&] {
            sepBy1([&] {
                U8 v = 0;

                if(token.type != LowerToken::Int) {
                    error("expected byte array initializer"_v);
                } else if(token.data.integer > 255 || token.data.integer < 0) {
                    warning("byte array initializer out of range"_v);
                } else {
                    v = token.data.integer;
                }

                contents.push(v);
                eat();
            }, LowerToken::Comma);
        });

        auto target = (U8*)module.arena.alloc(contents.size());
        copyMem((const U8*)contents.pointer(), target, contents.size());

        g->initialContents = { target, contents.size() };
    } else if(token.type == LowerToken::String) {
        auto string = context.findName(token.data.id);
        auto target = (U8*)module.arena.alloc(string.size());
        copyMem((const U8*)string.text(), target, string.size());

        g->initialContents = { target, string.size() };
        eat();
    } else if(token.type == LowerToken::Int || token.type == LowerToken::Long || token.type == LowerToken::Float || token.type == LowerToken::Double) {
        auto v = token.data;
        auto kind = token.type;
        auto type = LowerType::Int32;

        if(kind == LowerToken::Float) {
            type = LowerType::Float32;
        } else if(kind == LowerToken::Double) {
            type = LowerType::Float64;
        } else if(kind == LowerToken::Long) {
            type = LowerType::Int64;
        }

        eat();

        if(maybe(LowerToken::Colon)) {
            type = parseType();
        }

        // Always treat global pointers as being 8 bytes for now, to make compatibility easier.
        Size size = (type == LowerType::Int32 || type == LowerType::Float32) ? 4 : 8;
        auto target = (U8*)module.arena.alloc(size);
        g->initialContents = { target, size };

        if(type == LowerType::Int32) {
            U32 it = v.integer;
            copyMem(&it, target, size);
        } else if(type == LowerType::Int64 || type == LowerType::Pointer) {
            U64 it = v.integer;
            copyMem(&it, target, size);
        } else if(type == LowerType::Float32) {
            auto it = float(v.floating);
            copyMem(&it, target, size);
        } else if(type == LowerType::Float64) {
            double it = v.floating;
            copyMem(&it, target, size);
        } else {
            // A vector global would need as many numbers as it has lanes, which nothing writes one
            // of yet; a one-number initializer of one would silently be a splat or a first lane.
            error("a global cannot be initialized with a single number of this type"_v);
        }
    } else {
        error("expected global initializer"_v);
        return;
    }

    auto result = module.globals.add(g->name);
    if(!result.existed) module.globalOrder.push(g - *module.arena);
    if(result.existed) {
        diag.error("duplicate definition of global %@"_v, &g->source, context.findName(g->name));
    } else {
        *result.value = g - *module.arena;
    }
}

void LowerParser::parseDecl() {
    auto name = tryMaybe(expectNode(LowerToken::LabelID, "expected label identifier"_v), return);
    auto f = new (module.arena) LowerFunction(module.arena, &module, name.payload.id);
    f->source = context.addLocation(name.node);

    if(maybe(LowerToken::Less)) {
        auto convention = tryMaybe(expect(LowerToken::LabelID, "expected calling convention name"_v), return).id;

        if(auto type = callTypeForName(convention)) {
            f->callType = type.unwrap();
        } else {
            error("unknown calling convention"_v);
        }

        // And the markers, which the convention list carries rather than a second bracket: they are
        // all facts about how this one function is entered or emitted, and none is an instruction.
        // The convention stays mandatory when the brackets are written at all, so a marked function
        // with the default one is `f<complex, x86_legacy_sse>`.
        while(maybe(LowerToken::Comma)) {
            auto marker = tryMaybe(expect(LowerToken::LabelID, "expected a function marker"_v), return).id;

            if(marker == Context::nameHash(nameForLegacySse())) {
                f->legacyVectors = true;
            } else if(marker == Context::nameHash(nameForForeignBoundary())) {
                f->foreignBoundary = true;
            } else {
                error("unknown function marker - they are `x86_legacy_sse` and `foreign`"_v);
            }
        }

        tryMaybe(expect(LowerToken::Greater, "expected '>'"_v), return);
    }

    parens([&] {
        sepBy([&] {
            WithLocation location(*this);

            auto argName = tryMaybe(expect(LowerToken::RegID, "expected argument name"_v), return).id;
            tryMaybe(expect(LowerToken::Colon, "expected ':'"_v), return);

            auto arg = f->addArg(*module.arena, argName, parseType());
            arg->source = context.addLocation(location);
        }, LowerToken::Comma, LowerToken::ParenR);
    });

    if(maybe(LowerToken::Colon)) {
        sepBy1([&] {
            f->returnTypes.push(f->arena, parseType());
        }, LowerToken::Comma);
    }

    braces([&] {
        sepBy([&] {
            if(token.type == LowerToken::EndOfStmt || token.type == LowerToken::BraceR) return;
            parseBlock(f);
        }, LowerToken::EndOfStmt, LowerToken::BraceR);
    });

    auto result = module.functions.add(f->name);
    if(result.existed) {
        diag.error("duplicate definition of function %@"_v, f->source, context.findName(f->name));
    } else {
        *result.value = f - *module.arena;

        // Beside the map, because emission walks the list - see LowerModule. This path builds a
        // function without going through addFunction, so it has to say so itself.
        module.functionOrder.push(f - *module.arena);
    }
}

void LowerParser::parseBlock(LowerFunction* fun) {
    auto label = maybeNode(LowerToken::LabelID);
    auto block = fun->addBlock(*module.arena, label ? label.unwrap().payload.id : StringId());
    block->source = context.addLocation(label ? label.unwrap().node : currentNode());

    auto ast = new (buffer) LowerBlockAst;
    block->ast = ast - *buffer;

    braces([&] {
        sepBy([&] {
            if(token.type == LowerToken::EndOfStmt || token.type == LowerToken::BraceR) return;
            parseInst(*ast, false);
        }, LowerToken::EndOfStmt, LowerToken::BraceR);
    });
}

LowerInstAst* LowerParser::parseInst(LowerBlockAst& to, bool implicitResult) {
    auto inst = new (buffer) LowerInstAst(StringId());
    WithLocation location(*this);

    if(implicitResult) {
        inst->results.push(buffer, 0);
    } else if(token.type == LowerToken::RegID) {
        sepBy1([&] {
            auto targetName = tryMaybe(expect(LowerToken::RegID, "expected local binding"_v), return).id;

            // Add the result; the type is parsed later (if any).
            inst->results.push(buffer, encodeResultAst(targetName, Nothing()));
        }, LowerToken::Comma);

        expect(LowerToken::Equals, "expected local '=' after bindings"_v);
    }

    inst->inst = tryMaybe(expect(LowerToken::LabelID, "expected statement"_v), return nullptr).id;

    sepBy([&] {
        parseArg(inst, to);
    }, tokenEat(LowerToken::Comma), [&] {
        return token.type == LowerToken::EndOfStmt || token.type == LowerToken::Colon || token.type == LowerToken::ParenR;
    });

    if(maybe(LowerToken::Colon)) {
        Size i = 0;

        sepBy1([&] {
            auto type = parseType();

            if(inst->results.size() > i) {
                auto name = getResultName(inst->results[i]);
                inst->results.set(i, encodeResultAst(name, Just(type)));
            } else {
                error("invalid number of returned types"_v);
            }

            i++;
        }, LowerToken::Comma);
    }

    inst->source = context.addLocation(location);
    to.instructions.push(buffer, inst - *buffer);
    return inst;
}

void LowerParser::parseArg(LowerInstAst* ast, LowerBlockAst& to) {
    if(token.type == LowerToken::BracketL) {
        brackets([&] {
            auto label = tryMaybe(expect(LowerToken::LabelID, "expected phi label"_v), return).id;
            tryMaybe(expect(LowerToken::Comma, "expected ',' after phi label"_v), return);

            auto arg = parseBaseArg();
            arg.source = label;
            ast->args.push(buffer, arg);
        });
    } else if(token.type == LowerToken::ParenL) {
        parens([&] {
            auto index = parseInst(to, true);
            ast->args.push(buffer, LowerArgAst {
                .inst = index,
                .kind = LowerArgAst::Inst,
            });
        });
    } else {
        ast->args.push(buffer, parseBaseArg());
    }
}

LowerArgAst LowerParser::parseBaseArg() {
    if(token.type == LowerToken::LabelID || token.type == LowerToken::RegID || token.type == LowerToken::GlobalID) {
        auto id = token.data.id;
        auto type = LowerArgAst::Label;

        if(token.type == LowerToken::RegID) type = LowerArgAst::Reg;
        else if(token.type == LowerToken::GlobalID) type = LowerArgAst::Global;

        eat();

        return {
            .id = id,
            .kind = type,
        };
    } else if(token.type >= LowerToken::FirstLiteral && token.type <= LowerToken::LastLiteral) {
        return parseNumericArg();
    } else if(token.type == LowerToken::Minus) {
        eat();
        auto arg = parseNumericArg();

        if(arg.immType == LowerType::Int32 || arg.immType == LowerType::Int64) {
            arg.i = -arg.i;
        } else if(arg.immType == LowerType::Float32 || arg.immType == LowerType::Float64) {
            arg.f = -arg.f;
        } else {
            error("unknown numeric literal type"_v);
        }

        return arg;
    } else {
        error("expected argument"_v);
        return { .i = 0, .kind = LowerArgAst::Imm, .immType = LowerType::Int32 };
    }
}

LowerArgAst LowerParser::parseNumericArg() {
    auto type = token.type;

    if(type == LowerToken::Int || type == LowerToken::Long) {
        auto i = token.data.integer;
        eat();
        return { .i = i, .kind = LowerArgAst::Imm, .immType = type == LowerToken::Int ? LowerType::Int32 : LowerType::Int64 };
    } else if(type == LowerToken::Float || type == LowerToken::Double) {
        auto f = token.data.floating;
        eat();
        return { .f = f, .kind = LowerArgAst::Imm, .immType = type == LowerToken::Float ? LowerType::Float32 : LowerType::Float64 };
    } else {
        error("expected number"_v);
        return { .i = 0, .kind = LowerArgAst::Imm, .immType = LowerType::Int32 };
    }
}

/*
 * A vector or a mask, from the spelling the printer produces: `f32x8`, `i8x32`, `m32x8`.
 *
 * Read out of the identifier's text rather than compared against interned names, because there are
 * six lane kinds at seven lane counts and interning forty-odd names to recognize them would be a
 * second statement of what the printer already writes. Nothing here decides whether the type is one
 * this target can hold - a lane count that is not a power of two or a width no register has is
 * rejected here because it is not a *type*, and the rest is validateFunction's.
 */
static Maybe<LowerType> parseVectorTypeName(StringView name) {
    Size i = 0;
    auto digits = [&]() -> U32 {
        U32 value = 0;
        auto start = i;

        while(i < name.length && name.ptr[i] >= '0' && name.ptr[i] <= '9') {
            value = value * 10 + U32(name.ptr[i++] - '0');
            if(value > 1024) return 0;
        }

        return i == start ? 0 : value;
    };

    if(name.length < 4) return Nothing();
    auto kind = name.ptr[i++];
    if(kind != 'i' && kind != 'f' && kind != 'm') return Nothing();

    auto bits = digits();
    if(i >= name.length || name.ptr[i++] != 'x') return Nothing();

    auto lanes = digits();
    if(i != name.length) return Nothing();

    // A lane count is a power of two, at least two - a one-lane vector is a scalar and is spelled as
    // one - and the widest register this target describes is 64 bytes.
    if(lanes < 2 || (lanes & (lanes - 1)) != 0) return Nothing();
    if(U64(bits / 8) * lanes > 64) return Nothing();

    LowerLane lane;
    if(kind == 'f') {
        if(bits == 32) lane = LowerLane::Float32;
        else if(bits == 64) lane = LowerLane::Float64;
        else return Nothing();
    } else {
        if(bits == 8) lane = LowerLane::Int8;
        else if(bits == 16) lane = LowerLane::Int16;
        else if(bits == 32) lane = LowerLane::Int32;
        else if(bits == 64) lane = LowerLane::Int64;
        else return Nothing();
    }

    return Just(kind == 'm' ? maskType(lane, lanes) : vectorType(lane, lanes));
}

LowerType LowerParser::parseType() {
    auto i = tryMaybe(expect(LowerToken::LabelID, "expected type"_v), return LowerType::Int32).id;

    if(i == i32Id) return LowerType::Int32;
    if(i == i64Id) return LowerType::Int64;
    if(i == f32Id) return LowerType::Float32;
    if(i == f64Id) return LowerType::Float64;
    if(i == ptrId) return LowerType::Pointer;

    auto name = context.findName(i);
    auto vector = parseVectorTypeName(StringView { name.text(), name.size() });
    if(vector) return vector.unwrap();

    error("unknown type"_v);
    return LowerType::Int32;
}
