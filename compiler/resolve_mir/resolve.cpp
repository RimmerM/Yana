#include "resolve.h"
#include "build.h"
#include "verify.h"
#include "summary.h"

namespace mir {

namespace {

using ast::ParseBase;

/*
 * One operator this stage understands, and how tightly it binds.
 *
 * A stand-in for instance selection, and openly one. In the language `+` is `Num.add`, `<` is
 * `Ord.<` and `&&` is a `Truth` function with a `@lazy` second argument, so every row here is a
 * class member that milestone 6 will resolve through the ordinary overload machinery. What the
 * table buys until then is that a fixture can write arithmetic without a standard library, and what
 * it deliberately does not do is invent a rule: the precedences are `Core.yana`'s own fixity
 * declarations, so an expression groups here exactly as it does under the other stage.
 *
 * Every one of them is `infixl` in Core, which is why there is no associativity column. A right
 * associative operator would need one, and the climb below is written so that adding it is a
 * parameter rather than a restructuring.
 */
enum class OperatorForm: U8 {
    Binary,      // an ordinary two-operand computation
    ShortAnd,    // `&&` - the right operand runs only if the left was True
    ShortOr,     // `||` - the right operand runs only if the left was False
};

struct Operator {
    StringView text;
    U8 precedence;
    OperatorForm form;
    BinaryOp op;
};

static const Operator kOperators[] = {
    { "||"_v,  1, OperatorForm::ShortOr,  BinaryOp::Or  },
    { "&&"_v,  2, OperatorForm::ShortAnd, BinaryOp::And },
    { "=="_v,  3, OperatorForm::Binary,   BinaryOp::Eq  },
    { "!="_v,  3, OperatorForm::Binary,   BinaryOp::Ne  },
    { ">"_v,   3, OperatorForm::Binary,   BinaryOp::Gt  },
    { ">="_v,  3, OperatorForm::Binary,   BinaryOp::Ge  },
    { "<"_v,   3, OperatorForm::Binary,   BinaryOp::Lt  },
    { "<="_v,  3, OperatorForm::Binary,   BinaryOp::Le  },
    { "xor"_v, 5, OperatorForm::Binary,   BinaryOp::Xor },
    { "or"_v,  5, OperatorForm::Binary,   BinaryOp::Or  },
    { "and"_v, 6, OperatorForm::Binary,   BinaryOp::And },
    { "shl"_v, 6, OperatorForm::Binary,   BinaryOp::Shl },
    { "shr"_v, 6, OperatorForm::Binary,   BinaryOp::Shr },
    { "sar"_v, 6, OperatorForm::Binary,   BinaryOp::Sar },
    { "+"_v,   7, OperatorForm::Binary,   BinaryOp::Add },
    { "-"_v,   7, OperatorForm::Binary,   BinaryOp::Sub },
    { "*"_v,   8, OperatorForm::Binary,   BinaryOp::Mul },
    { "/"_v,   8, OperatorForm::Binary,   BinaryOp::Div },
    { "%"_v,   8, OperatorForm::Binary,   BinaryOp::Rem },
};

/*
 * An infix chain, flattened.
 *
 * The parser nests infix expressions to the right without regard for precedence, because fixity is
 * a module-level property it cannot know. So the chain is flattened whole and re-associated by
 * `climb`, which is the same two steps the current resolver takes for the same reason.
 */
struct Chain {
    SmallArray<ast::Expr*, 8> operands;
    SmallArray<const Operator*, 8> operators;
    SmallArray<LocationId, 8> sources;
};

// A name in scope inside a body, and the storage it names.
struct Binding {
    StringId name;
    Local* local;
};

/*
 * One module being resolved.
 *
 * The two passes are separated because a `let` may name a function and a function may name a `let`
 * declared after it: what a module declares is a property of the module, not of the order a reader
 * arrives at it in. That is the same rule the current resolver has and one of the few things about
 * it this rewrite reproduces rather than reconsiders.
 */
struct Resolver {
    Context& context;
    Program& program;
    ParseBase parse;
    Module& module;

    /*
     * One entry per *written* declaration, including the ones the first pass rejected.
     *
     * `Module::functions` and `Module::globals` hold only what was accepted, because they are what
     * emission walks. These two are what the second pass pairs bodies and initializers against, and
     * they need the holes: a rejected `fn` that contributed no entry would make every declaration
     * after it resolve its body against the wrong signature, and the diagnostics that produces are
     * about names in a function nobody wrote.
     */
    Array<DeclPtr<Function>> writtenFunctions;
    Array<DeclPtr<Global>> writtenGlobals;

    /*
     * The names in scope in the body being resolved, innermost last.
     *
     * A flat array with the scope boundary kept on the C++ stack rather than a tree of maps: a
     * lookup walks backwards and stops at the first match, which is shadowing, and leaving a scope
     * is truncating the array to the length that was saved on entry. Bodies are small enough that
     * the linear walk is the cheaper structure, and it is the one that makes shadowing fall out
     * rather than being implemented.
     */
    Array<Binding> bindings;

    DeclBase decl() { return *program.decl; }

    void unsupported(LocationId source, StringView what) {
        context.diagnostics.error("the ownership resolver does not implement %@ yet"_v, source, what);
    }

    /*
     * A written type.
     *
     * Only the builtin widths, `Bool`, `{}` and `%T` at this milestone. A named type that is not a
     * builtin is a declaration this stage has no table for yet, and saying so by name is more useful
     * than an "unknown type" that could equally mean a typo.
     */
    DeclPtr<Type> resolveType(ast::Type& type) {
        switch(type.kind) {
            case ast::Type::Unit:
                return program.types.unit;
            case ast::Type::Con: {
                if(auto builtin = program.types.findBuiltin(context, type.name)) return builtin;

                context.diagnostics.error("unknown type %@ - the ownership resolver knows only the builtin widths so far"_v,
                                          type.source, context.findName(type.name));
                return nullptr;
            }
            case ast::Type::Ptr: {
                auto to = resolveType(*parse[type.to]);
                return to ? program.types.pointerType(to) : nullptr;
            }
            default:
                unsupported(type.source, "this type form"_v);
                return nullptr;
        }
    }

    /*
     * A constant expression, which is what a global's initializer is.
     *
     * `hint` is the type the position asks for, and it is an input rather than a check afterwards:
     * `42` has no width of its own, and `let x = 42 :: I32` says what it is by ascription rather than
     * by having a later use decide. Inference here is deliberately local, the way the language's is.
     */
    Constant resolveConstant(ast::Expr& expr, DeclPtr<Type> hint) {
        // A literal's kind is `Lit` plus the literal kind, so it is a range rather than one label -
        // see `ast::Expr::Lit`, which the AST requires to be last for exactly this.
        if(expr.kind >= ast::Expr::Lit) return resolveLiteral(expr, hint, false);

        switch(expr.kind) {
            case ast::Expr::Coerce: {
                auto& coerce = *parse[expr.coerce];
                auto type = resolveType(coerce.type);
                if(!type) return Constant();

                return resolveConstant(coerce.target, type);
            }
            case ast::Expr::Nested:
                return resolveConstant(*parse[expr.nested], hint);
            case ast::Expr::Con:
                return resolveBoolean(expr, hint);

            /*
             * `-1` is a written negative number, not a negation of a positive one.
             *
             * The lexer produces the magnitude alone, so folding the sign in here is what makes
             * `I64`'s own minimum expressible: negating it after the fact is signed overflow, and
             * `resolve/const.cpp` records that having been the bug on the other side.
             */
            case ast::Expr::Prefix: {
                auto& prefix = *parse[expr.prefix];
                if(prefix.op.kind == ast::Expr::Var && stringView(context.findName(prefix.op.var)) == "-"_v &&
                   prefix.on.kind >= ast::Expr::Lit) {
                    return resolveLiteral(prefix.on, hint, true);
                }

                unsupported(expr.source, "this constant expression"_v);
                return Constant();
            }
            default:
                unsupported(expr.source, "this constant expression"_v);
                return Constant();
        }
    }

    // `True` and `False`, which are nullary constructors rather than literal tokens - see
    // `ast::Literal::Bool`, which the lexer never produces. Core declares them; this stage has the
    // type builtin and recognizes the two names, which is the same shortcut `Bool` itself is.
    Constant resolveBoolean(ast::Expr& expr, DeclPtr<Type> hint) {
        auto& written = *parse[expr.con];

        if(written.args.size() || written.type.kind != ast::Type::Con) {
            unsupported(expr.source, "constructors"_v);
            return Constant();
        }

        auto text = stringView(context.findName(written.type.name));
        if(text != "True"_v && text != "False"_v) {
            unsupported(expr.source, "constructors"_v);
            return Constant();
        }

        if(hint && decl()[hint]->kind != Type::Bool) {
            context.diagnostics.error("%@ is a Bool, and this position asks for something else"_v,
                                      expr.source, text);
            return Constant();
        }

        return boolConstant(program.types.boolean, text == "True"_v);
    }

    Constant resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative) {
        auto type = hint;
        auto& literal = expr.lit;

        switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
            case ast::Literal::Int: {
                // The default width where nothing asked for one. `I32` rather than the widest,
                // because that is what Core's `Int` is an alias for, and a literal that silently
                // widened would make two programs out of one source.
                if(!type) type = program.types.intType(32, true);

                if(decl()[type]->kind != Type::Int) {
                    context.diagnostics.error("an integer literal cannot have this type"_v, expr.source);
                    return Constant();
                }

                auto value = literal.i();
                return intConstant(type, negative ? U64(0) - value : value);
            }
            case ast::Literal::Double:
            case ast::Literal::Float: {
                if(!type) type = program.types.floatType(64);

                if(decl()[type]->kind != Type::Float) {
                    context.diagnostics.error("a floating-point literal cannot have this type"_v, expr.source);
                    return Constant();
                }

                auto value = literal.d();
                return floatConstant(type, negative ? -value : value);
            }
            default:
                unsupported(expr.source, "this literal"_v);
                return Constant();
        }
    }

    /*
     * Pass one: what the module declares.
     */

    void declareGlobals(ast::Decl& declaration) {
        if(declaration.stmt.kind != ast::Expr::Decl) {
            unsupported(declaration.source, "a module-level statement"_v);
            return;
        }

        for(auto binding: declaration.stmt.decl.contents(parse)) {
            if(binding.pat.kind != ast::Pat::Var) {
                context.diagnostics.error("a global must be declared as a single name"_v, binding.pat.source);
                writtenGlobals.push(nullptr);
                continue;
            }

            if(!binding.content) {
                context.diagnostics.error("a global requires an initializer"_v, binding.pat.source);
                writtenGlobals.push(nullptr);
                continue;
            }

            auto global = new (program.decl) Global(binding.pat.var, owned(nullptr));
            global->source = binding.pat.source;
            global->writable = binding.bind == ast::BindType::Ref;

            auto pointer = global - *program.decl;
            module.globals.push(program.decl, pointer);
            writtenGlobals.push(pointer);
            if(!program.addGlobalName(&module - *program.decl, global->name, pointer)) {
                context.diagnostics.error("%@ is already declared in this module"_v,
                                          binding.pat.source, context.findName(global->name));
            }
        }
    }

    /*
     * One written parameter's contract - Analysis-Borrows-Revised.md §2.1.
     *
     * The three spellings and what each one grants:
     *
     *  - `x: T` is shared, call-bounded access. The callee may read the value for the length of the
     *    call and may not keep it.
     *  - `&x: T` is unique, call-bounded access: exclusive, and writable through the parameter.
     *  - `->x: T` is ownership, transferred to the callee, which is then what has to end it.
     *
     * `&` is refused here and not implemented, because it is a *loan* the caller has to grant over a
     * place it keeps, and loans are milestone 5. A shared parameter of a trivially copyable type is
     * the one borrow this milestone can honour without them, and it can because a trivial value's
     * loan and its copy are physically the same thing - which is precisely why the two must stay
     * distinct in the IR even here.
     *
     * A `'` on the type publishes an origin slot and is refused for the same reason.
     */
    bool argContract(ast::Arg& written, Contract& into) {
        switch(written.bind) {
            case ast::BindType::Borrow:
                into = borrowedContract(Access::Shared);
                return true;
            case ast::BindType::Sink:
                into = ownedContract();
                return true;
            case ast::BindType::Ref:
                unsupported(written.source, "an `&` parameter"_v);
                return false;
        }

        unsupported(written.source, "this parameter form"_v);
        return false;
    }

    /*
     * The signature's parameters, in order.
     *
     * Answers false when the declaration is one this stage cannot represent, in which case the
     * function is dropped whole rather than left with a signature a call would be checked against.
     * A partially declared signature is worse than none: every call to it reports against positions
     * the author did not write.
     */
    bool declareArgs(Function& function, ast::Decl& declaration) {
        auto& written = declaration.fun;
        auto ok = true;

        for(auto argument: written.args.contents(parse)) {
            if(argument.def) {
                unsupported(argument.source, "a parameter default"_v);
                ok = false;
                continue;
            }

            if(argument.lazy) {
                unsupported(argument.source, "a `@lazy` parameter"_v);
                ok = false;
                continue;
            }

            if(argument.caller) {
                unsupported(argument.source, "a `@caller` parameter"_v);
                ok = false;
                continue;
            }

            // `return &a: T` - the result may be rooted in this argument, which is an origin slot in
            // the signature's schema and needs the loan machinery to mean anything.
            if(argument.returnRoot) {
                unsupported(argument.source, "a `return` parameter"_v);
                ok = false;
                continue;
            }

            if(!argument.type) {
                unsupported(argument.source, "an inferred parameter type"_v);
                ok = false;
                continue;
            }

            auto type = resolveType(*parse[argument.type]);
            Contract contract;
            if(!type || !argContract(argument, contract)) {
                ok = false;
                continue;
            }

            /*
             * Two parameters of one name.
             *
             * Refused rather than shadowed. `findLocal` walks the bindings backwards, so the second
             * would win every mention of the name in the body and the first would be a parameter the
             * callee cannot read at all - which is a silent wrong answer rather than a missing
             * feature. A linear scan because a signature is short and a set would be an allocation
             * per declaration to save it.
             */
            auto duplicate = false;
            for(auto existing: function.args.contents(*program.decl)) {
                if((*program.decl)[existing]->name != argument.name) continue;

                context.diagnostics.error("this function already has a parameter called %@"_v,
                                          argument.source, context.findName(argument.name));
                duplicate = true;
                break;
            }

            if(duplicate) {
                ok = false;
                continue;
            }

            auto declared = new (program.decl) Arg();
            declared->name = argument.name;
            declared->occurrence = Occurrence { type, contract, nullptr };
            declared->source = argument.source;
            declared->index = U32(function.args.size());

            function.args.push(program.decl, declared - *program.decl);
        }

        return ok;
    }

    void declareFunction(ast::Decl& declaration) {
        auto& written = declaration.fun;

        if(written.kind != ast::FunKind::Plain) {
            unsupported(declaration.source, "lens and iterator functions"_v);
            writtenFunctions.push(nullptr);
            return;
        }

        if(written.constraints.size()) {
            unsupported(declaration.source, "generic functions"_v);
            writtenFunctions.push(nullptr);
            return;
        }

        auto function = new (program.decl) Function(written.name, &module - *program.decl);
        function->source = declaration.source;

        if(!declareArgs(*function, declaration)) {
            writtenFunctions.push(nullptr);
            return;
        }

        /*
         * `-> ->T` - a written result convention, which this stage has no representation for.
         *
         * Refused rather than ignored. The marker is a fact about what the *receiver* gets, and
         * Analysis-Lens.md §3.3 is where it means something: a lens hands a value to its
         * continuation under an argument-like contract, and `->` there is the hand-over. A plain
         * function's result is owned by the caller either way, so silently dropping it would be
         * right today and wrong the moment the two spellings differ - and nothing would say when.
         */
        if(written.retBind != ast::BindType::Borrow) {
            unsupported(declaration.source, "a written result convention"_v);
            writtenFunctions.push(nullptr);
            return;
        }

        // Absent, which is `{}` - a `:` body returns nothing and writes no result type at all. The
        // occurrence is owned, because a result the caller receives is theirs.
        auto result = written.ret ? resolveType(*parse[written.ret]) : program.types.unit;
        function->result = owned(result);

        /*
         * The one fact that is settled at declaration time, and exactly one.
         *
         * `bodyKnown` is about the *declaration* - whether there is anything to analyse - and it is
         * the only summary field a declaration answers by itself. Suspension used to be set here as
         * a `Known` false, which was true only while nothing could call anything; it is now what
         * `computeSummaries` publishes, over the call graph the bodies produce. Every other fact
         * stays `Unknown` until its own pass exists, which is what `Fact` is for.
         */
        function->summary.bodyKnown = written.body != nullptr;

        auto pointer = function - *program.decl;
        module.functions.push(program.decl, pointer);
        writtenFunctions.push(pointer);

        /*
         * One declaration per name, which is this milestone's stand-in for an overload set.
         *
         * doc/spec/overloading.md keys a set by `(name, arity)` and admits one plain function per
         * key beside any number of class functions, and selecting among those is milestone 6. Until
         * then a second declaration would be one the name table silently kept the first of - a
         * function nothing could call and no message about it.
         */
        if(!program.addFunctionName(&module - *program.decl, function->name, pointer)) {
            unsupported(declaration.source, "two functions of one name"_v);
        }
    }

    void declare(ast::Module& file) {
        for(auto declaration: file.decls.contents(parse)) {
            switch(declaration.kind) {
                case ast::Decl::Stmt:
                    declareGlobals(declaration);
                    break;
                case ast::Decl::Fun:
                    declareFunction(declaration);
                    break;
                case ast::Decl::Error:
                    break;
                default:
                    unsupported(declaration.source, "this declaration"_v);
                    break;
            }
        }
    }

    /*
     * Pass two: the initializers and the bodies.
     */

    void resolveGlobals(ast::Decl& declaration, Size& next) {
        if(declaration.stmt.kind != ast::Expr::Decl) return;

        for(auto binding: declaration.stmt.decl.contents(parse)) {
            if(next >= writtenGlobals.size()) return;

            auto pointer = writtenGlobals[next++];
            if(!pointer || !binding.content) continue;

            auto global = decl()[pointer];
            auto constant = resolveConstant(*parse[binding.content], nullptr);
            if(!constant.isSet()) continue;

            global->initial = constant;
            global->occurrence = owned(constant.type);
        }
    }

    /*
     * Scopes.
     */

    Local* findLocal(StringId name) {
        for(Size i = bindings.size(); i > 0; i--) {
            if(bindings[i - 1].name == name) return bindings[i - 1].local;
        }

        return nullptr;
    }

    /*
     * Types.
     */

    // Whether a node produced the type this position asked for. One rule and one message, because
    // "the wrong type arrived here" is the same failure wherever it happens and a reader comparing
    // two diagnostics about it should not have to notice they are two.
    bool checkType(Node& node, DeclPtr<Type> expected, LocationId source, StringView what) {
        if(sameType(valueType(node), expected)) return true;

        context.diagnostics.error("%@ must be %@, and this is %@"_v, source, what,
                                  typeText(expected), typeText(valueType(node)));
        return false;
    }

    // A type as the text a diagnostic names it by. `printType` writes into a `Net::Writer`, which is
    // the dump's interface rather than a message's, so the short spellings are written out here.
    String typeText(DeclPtr<Type> type) {
        if(!type) return String("?");

        auto& logical = *decl()[type];
        switch(logical.kind) {
            case Type::Unit:    return String("{}");
            case Type::Bool:    return String("Bool");
            case Type::Int:
                return Tritium::format("%@%@", ((IntType&)logical).isSigned ? "I"_v : "U"_v,
                                       U64(((IntType&)logical).bits));
            case Type::Float:
                return Tritium::format("F%@", U64(((FloatType&)logical).bits));
            case Type::Pointer:
                return Tritium::format("%%%@", typeText(((PointerType&)logical).to));
            case Type::KindCount:
                break;
        }

        return String("?");
    }

    /*
     * How a value is read out of a place, for a position that may consume it.
     *
     * A read is what turns storage into a value; what the consumer then does with that value is its
     * own operand's mode, which is why this answers from the *place* and never from the position.
     * Three answers and one rule each: a trivially copyable type is copied, storage this function
     * does not own can only be borrowed from, and an owned non-trivial place is a `Forward`
     * candidate - the bare owned name that the language moves when it is dead afterwards. The
     * candidate is what §5.4 insists on: source order is not liveness, so the builder records the
     * question rather than guessing, and liveness answers it.
     */
    UseMode readModeFor(Place& place) {
        // `isTriviallyCopyable` and nothing else, because a second reading of "trivial" is a second
        // answer: this used to test the kinds by hand and would have disagreed with the verifier -
        // which asks that function - the first time a type had a teardown and no copy, or a copy and
        // no teardown.
        if(isTriviallyCopyable(decl(), place.occurrence.type)) return UseMode::CopyTrivial;

        // Storage this function does not own has nothing to hand over, whatever the position wanted:
        // a `Forward` out of a shared parameter would be a move out of the caller's value. The loan
        // this produces is what milestone 5 gives an extent to.
        if(!place.occurrence.contract.isOwned()) return UseMode::BorrowShared;

        return UseMode::Forward;
    }

    /*
     * Expressions.
     */

    /*
     * A body expression, in a position that wants a value.
     *
     * `hint` is what the position asks for and is an input to inference rather than a check after
     * it, for the reason `resolveConstant` gives: a literal has no width of its own. Null is
     * returned when there is no value here, which is either an error already reported or control
     * having left - `Builder::isTerminated` is what tells a caller which, and every caller has to
     * ask before it emits anything else.
     */
    Node* resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
        switch(expr.kind) {
            case ast::Expr::Nested:
                return resolveValue(builder, *parse[expr.nested], hint);
            case ast::Expr::Var:
                return resolveName(builder, expr, expr.var);
            case ast::Expr::Con: {
                auto constant = resolveBoolean(expr, hint);
                return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
            }
            case ast::Expr::Coerce: {
                /*
                 * `e :: T` - an ascription, which is a hint and then a check.
                 *
                 * Not a conversion: `::` says what a value is and `convert` is what changes one.
                 * Passing the type down first is what makes `1 :: I64` one instruction rather than a
                 * widened `I32`, and checking afterwards is what makes `x :: I64` on an `I32` local
                 * a diagnostic rather than a silent extension.
                 */
                auto& coerce = *parse[expr.coerce];
                auto type = resolveType(coerce.type);
                if(!type) return nullptr;

                auto value = resolveValue(builder, coerce.target, type);
                if(!value) return nullptr;

                /*
                 * `::` widens as well as ascribes, and only the ascribing half is here.
                 *
                 * `x :: I64` on an `I32` is a legal widening in the language, so a plain "wrong
                 * type" would be describing the program wrongly rather than describing this stage.
                 * Conversions are a class - `Convert` - and arrive with instance selection; what
                 * this milestone can do honestly is name the gap.
                 */
                if(!sameType(valueType(*value), type)) {
                    auto text = Tritium::format("converting %@ to %@ with `::`",
                                                typeText(valueType(*value)), typeText(type));
                    unsupported(expr.source, stringView(text));
                    return nullptr;
                }

                return value;
            }
            case ast::Expr::Infix:
                return resolveInfix(builder, expr, hint);
            case ast::Expr::Prefix:
                return resolvePrefix(builder, expr, hint);
            case ast::Expr::App:
                return resolveCall(builder, expr);
            case ast::Expr::If:
                return resolveIf(builder, expr, hint, true);
            case ast::Expr::Multi:
                return resolveBlock(builder, expr, hint, true);
            default:
                if(expr.kind >= ast::Expr::Lit) {
                    auto constant = resolveLiteral(expr, hint, false);
                    return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
                }

                unsupported(expr.source, "this expression"_v);
                return nullptr;
        }
    }

    // A name, as the read of the place it stands for. Locals shadow globals, which is what makes the
    // backwards walk in `findLocal` the whole of the rule.
    Node* resolveName(Builder& builder, ast::Expr& expr, StringId name) {
        if(auto local = findLocal(name)) {
            auto place = builder.localPlace(*local);
            return builder.addRead(expr.source, *place, readModeFor(*place));
        }

        auto found = program.findGlobal(&module - *program.decl, name);
        if(!found) {
            context.diagnostics.error("unknown name %@"_v, expr.source, context.findName(name));
            return nullptr;
        }

        // Its initializer was rejected, so it has no type. Every path that produces this has already
        // reported; what this stops is the *second* failure, which is a read with nothing to read.
        if(!decl()[found]->occurrence.type) return nullptr;

        auto place = builder.globalPlace(found);
        return builder.addRead(expr.source, *place, readModeFor(*place));
    }

    /*
     * A call - Analysis-IR.md §5.6.
     *
     * Three steps and no more: find the callee, resolve one argument per declared position, and hand
     * the values to `Builder::addCall`, which is where the signature becomes argument modes. The
     * resolver deliberately never states a mode - see `argumentMode` - so what is left here is name
     * resolution and the arity and type diagnostics, which are the two failures a *program* has.
     *
     * Selection is by name alone, which is this milestone's stand-in for §11.4's sixth: an overload
     * set is keyed by name and arity and resolved against logical types, and until instance
     * selection exists a second declaration of one name is refused at its declaration rather than
     * silently shadowed here.
     */
    Node* resolveCall(Builder& builder, ast::Expr& expr) {
        auto& written = *parse[expr.app];

        if(written.callee.kind != ast::Expr::Var) {
            unsupported(written.callee.source, "calling anything but a named function"_v);
            return nullptr;
        }

        auto name = written.callee.var;

        // A local of the same name is a *value*, and calling one is an indirect call through a
        // function value - a different node with a different summary rule (Analysis-Sharing.md §3.5)
        // and not something to quietly resolve to the declaration it shadows.
        if(findLocal(name)) {
            unsupported(written.callee.source, "calling a value"_v);
            return nullptr;
        }

        auto found = program.findFunction(&module - *program.decl, name);
        if(!found) {
            context.diagnostics.error("unknown function %@"_v, written.callee.source,
                                      context.findName(name));
            return nullptr;
        }

        auto& callee = *decl()[found];

        // A declaration with no body is a call this stage cannot emit. It is separate from the
        // summary's own answer for one - `bodyKnown` false publishes "may suspend" and is the right
        // fact - because what stops here is code generation rather than analysis.
        if(!callee.summary.bodyKnown) {
            unsupported(expr.source, "calling a function with no body"_v);
            return nullptr;
        }

        if(!checkCallShape(written, callee, expr.source)) return nullptr;

        SmallArray<Node*, 8> arguments;
        Size index = 0;

        for(auto argument: written.args.contents(parse)) {
            auto& parameter = *decl()[callee.args.get(decl(), U32(index++))];

            auto value = resolveValue(builder, argument.value, parameter.occurrence.type);
            if(!value) return nullptr;

            if(!checkType(*value, parameter.occurrence.type, argument.value.source,
                          "an argument"_v)) {
                return nullptr;
            }

            arguments.push(value);
        }

        return builder.addCall(expr.source, callee, arguments.pointer(), arguments.size());
    }

    /*
     * Whether the call's shape matches the signature at all, before any argument is resolved.
     *
     * Both questions are about the call as a whole and neither leaves a position to carry on from: a
     * named argument changes which parameter every later position fills, and a wrong count means
     * every position after the first missing one is checked against the wrong type. Resolving the
     * arguments first and reporting afterwards would produce one true diagnostic and a run of
     * consequences of it.
     */
    bool checkCallShape(ast::AppExpr& written, Function& callee, LocationId source) {
        for(auto argument: written.args.contents(parse)) {
            if(argument.name) {
                unsupported(argument.value.source, "a named argument"_v);
                return false;
            }
        }

        if(written.args.size() != callee.args.size()) {
            context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source,
                                      context.findName(callee.name), U64(callee.args.size()),
                                      U64(written.args.size()));
            return false;
        }

        return true;
    }

    /*
     * `-x` and `!x`.
     *
     * A negated literal is folded into the constant rather than emitted as a negation - see
     * `resolveConstant`, which does the same for a global's initializer and says why it matters.
     */
    Node* resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
        auto& written = *parse[expr.prefix];

        if(written.op.kind != ast::Expr::Var) {
            context.diagnostics.error("a prefix operator must be named"_v, written.op.source);
            return nullptr;
        }

        auto text = stringView(context.findName(written.op.var));

        if(text == "-"_v && written.on.kind >= ast::Expr::Lit) {
            auto constant = resolveLiteral(written.on, hint, true);
            return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
        }

        UnaryOp op;
        if(text == "-"_v) op = UnaryOp::Neg;
        else if(text == "!"_v) op = UnaryOp::Not;
        else {
            unsupported(written.op.source, "this prefix operator"_v);
            return nullptr;
        }

        // `!` asks for a `Bool` and `-` passes the position's hint through, since negation answers
        // what it was given.
        auto operandHint = op == UnaryOp::Not ? program.types.boolean : hint;
        auto value = resolveValue(builder, written.on, operandHint);
        if(!value) return nullptr;

        if(!operandTypeAllowed(decl(), unaryCategory(op), valueType(*value))) {
            context.diagnostics.error("%@ cannot be applied to %@"_v, expr.source, text,
                                      typeText(valueType(*value)));
            return nullptr;
        }

        return builder.addUnary(expr.source, op, *value);
    }

    const Operator* findOperator(StringId name) {
        auto text = stringView(context.findName(name));
        for(auto& entry: kOperators) {
            if(text == entry.text) return &entry;
        }

        return nullptr;
    }

    /*
     * An infix chain: flattened, then re-associated.
     *
     * The parser nests to the right without regard for precedence, so `a + b * c` arrives as
     * `a + (b * c)` only by accident of the tokens. Flattening the whole chain first and climbing it
     * afterwards is what makes the grouping the fixity table's answer rather than the parser's.
     */
    Node* resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
        Chain chain;
        auto node = parse[expr.infix];

        while(true) {
            if(node->op.kind != ast::Expr::Var) {
                context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
                return nullptr;
            }

            auto found = findOperator(node->op.var);
            if(!found) {
                unsupported(node->op.source, "this operator"_v);
                return nullptr;
            }

            chain.operands.push(&node->lhs);
            chain.operators.push(found);
            chain.sources.push(LocationId(node->op.source));

            if(node->rhs.kind != ast::Expr::Infix) {
                chain.operands.push(&node->rhs);
                break;
            }

            node = parse[node->rhs.infix];
        }

        Size operandIndex = 0;
        Size operatorIndex = 0;
        return climb(builder, chain, operandIndex, operatorIndex, 0, hint);
    }

    /*
     * Precedence climbing over a flattened chain.
     *
     * `hint` reaches the left operand and, for the operations that answer their operand type, the
     * right one takes the left one's answer instead. That is where this stage's inference stops:
     * `x + 1` types the literal from `x`, and `1 + x` types `x` from a literal that defaulted, which
     * is a real limit rather than an oversight. Milestone 3's signatures are what a position's type
     * will come from; local inference is what the language specifies and what is here.
     */
    Node* climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                U8 minimumPrecedence, DeclPtr<Type> hint) {
        auto lhs = resolveValue(builder, *chain.operands[operandIndex++], hint);

        while(operatorIndex < chain.operators.size() &&
              chain.operators[operatorIndex]->precedence >= minimumPrecedence) {
            auto op = chain.operators[operatorIndex];
            auto source = chain.sources[operatorIndex];
            operatorIndex++;

            // One rung above this operator's own, which is what makes a left associative operator
            // stop before an equal one and take it as its own left operand instead.
            auto next = U8(op->precedence + 1);

            if(op->form != OperatorForm::Binary) {
                lhs = shortCircuit(builder, chain, operandIndex, operatorIndex, next, source, lhs, *op);
                if(!lhs) return nullptr;
                continue;
            }

            if(!lhs) return nullptr;

            auto rhs = climb(builder, chain, operandIndex, operatorIndex, next, valueType(*lhs));
            if(!rhs) return nullptr;

            lhs = applyBinary(builder, source, op->op, *lhs, *rhs);
            if(!lhs) return nullptr;
        }

        return lhs;
    }

    Node* applyBinary(Builder& builder, LocationId source, BinaryOp op, Node& lhs, Node& rhs) {
        auto type = valueType(lhs);

        if(!sameType(type, valueType(rhs))) {
            context.diagnostics.error("%@ compares %@ with %@, which are different types"_v, source,
                                      binaryOpName(op), typeText(type), typeText(valueType(rhs)));
            return nullptr;
        }

        if(!operandTypeAllowed(decl(), binaryCategory(op), type)) {
            context.diagnostics.error("%@ cannot be applied to %@"_v, source, binaryOpName(op),
                                      typeText(type));
            return nullptr;
        }

        return builder.addBinary(source, op, lhs, rhs);
    }

    /*
     * `&&` and `||`, which are control flow rather than operations.
     *
     * `a && b` is `if a then b else False` and `a || b` is `if a then True else b`, which is what
     * Core's own definitions say they are - the difference being that Core writes them as functions
     * over `Truth` with a `@lazy` second argument, and here the laziness is the branch itself.
     * Building them out of the same merge every `if` uses is the point: the right operand is a span
     * of the chain rather than an AST node anyone can point at, and climbing it *inside* the block
     * it belongs to is what makes it run only when it should.
     */
    Node* shortCircuit(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                       U8 next, LocationId source, Node* condition, const Operator& op) {
        if(!condition) return nullptr;
        if(!checkType(*condition, program.types.boolean, source, "a short-circuit operand"_v)) return nullptr;

        auto shortValue = op.form == OperatorForm::ShortAnd ? false : true;

        return branchValue(builder, source, *condition, program.types.boolean,
            [&](DeclPtr<Type>) -> Node* {
                // The arm that runs the right operand. `ShortAnd` reaches it when the left was True.
                if(op.form == OperatorForm::ShortAnd) {
                    return climbBool(builder, chain, operandIndex, operatorIndex, next, source);
                }

                return constantBool(builder, source, shortValue);
            },
            [&](DeclPtr<Type>) -> Node* {
                if(op.form == OperatorForm::ShortAnd) return constantBool(builder, source, shortValue);
                return climbBool(builder, chain, operandIndex, operatorIndex, next, source);
            });
    }

    Node* climbBool(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                    U8 next, LocationId source) {
        auto value = climb(builder, chain, operandIndex, operatorIndex, next, program.types.boolean);
        if(!value) return nullptr;
        if(!checkType(*value, program.types.boolean, source, "a short-circuit operand"_v)) return nullptr;

        return value;
    }

    Node* constantBool(Builder& builder, LocationId source, bool value) {
        return builder.addConst(source, boolConstant(program.types.boolean, value));
    }

    /*
     * The shape `if`, `&&` and `||` share: two arms, and a merge that takes what they produced.
     *
     * The jumps are emitted *after* both arms have been resolved, and that is not tidiness. The
     * merge's parameter list has to exist before an edge can supply it and its type is what the arms
     * decide, so the block each arm ended in is remembered and returned to. `Node::id` is creation
     * order rather than position, so a jump built late and standing early is exactly as verifiable.
     *
     * An arm that left by itself - a `return`, or an error already reported - supplies nothing, and
     * the merge is then reached from one side or from neither. Neither is `unreachable`: the join
     * really is, and saying so is what keeps the caller from appending to a block nothing enters.
     */
    template<class T, class F>
    Node* branchValue(Builder& builder, LocationId source, Node& condition, DeclPtr<Type> hint,
                      T&& whenTrue, F&& whenFalse) {
        auto thenBlock = builder.addBlock(StringId());
        auto elseBlock = builder.addBlock(StringId());
        auto join = builder.addBlock(StringId());

        builder.addBranch(source, builder.resultOf(condition, UseMode::Inspect), *thenBlock, *elseBlock);

        builder.setBlock(*thenBlock);
        auto thenValue = whenTrue(hint);
        auto thenEnd = builder.currentBlock();
        auto thenLive = thenValue && !builder.isTerminated();

        builder.setBlock(*elseBlock);
        auto elseValue = whenFalse(thenValue ? valueType(*thenValue) : hint);
        auto elseEnd = builder.currentBlock();
        auto elseLive = elseValue && !builder.isTerminated();

        if(thenLive && elseLive &&
           !sameType(valueType(*thenValue), valueType(*elseValue))) {
            context.diagnostics.error("the two arms answer %@ and %@, which are different types"_v,
                                      source, typeText(valueType(*thenValue)),
                                      typeText(valueType(*elseValue)));
            elseLive = false;
        }

        Node* argument = nullptr;
        if(thenLive || elseLive) {
            auto type = thenLive ? valueType(*thenValue) : valueType(*elseValue);
            argument = builder.addBlockArgument(*join, owned(type), source);
        }

        closeArm(builder, *thenEnd, *join, thenLive ? thenValue : nullptr, source);
        closeArm(builder, *elseEnd, *join, elseLive ? elseValue : nullptr, source);

        builder.setBlock(*join);
        if(!argument) builder.addUnreachable(source);

        return argument;
    }

    // One arm's exit: the jump that hands its value over, or nothing where the arm already left.
    void closeArm(Builder& builder, Block& end, Block& join, Node* value, LocationId source) {
        builder.setBlock(end);
        if(builder.isTerminated()) return;

        if(!value) {
            // The arm produced no value and did not leave, which only happens after a diagnostic.
            // `unreachable` rather than a jump, because a merge it entered would need a parameter it
            // has nothing to supply.
            builder.addUnreachable(source);
            return;
        }

        // `Take` rather than a `Forward` candidate: what is being handed over is a node result,
        // which is a value already in hand and has no other last use to weigh it against. `Forward`
        // belongs to a bare *name* in sink position - see UseMode.
        auto argument = builder.resultOf(*value, UseMode::Take);
        builder.addJump(source, join, &argument, 1);
    }

    /*
     * `if c then a else b`, as a value or as a statement.
     *
     * One function for both, because the difference is only what the arms are resolved as and
     * whether the merge takes a parameter. A statement `if` with no `else` still branches to a merge
     * from both sides, so the block structure a reader sees does not change with the presence of a
     * value.
     */
    Node* resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue) {
        auto& written = *parse[expr.singleIf];

        auto condition = resolveValue(builder, written.cond, program.types.boolean);
        if(!condition) return nullptr;
        if(!checkType(*condition, program.types.boolean, written.cond.source, "a condition"_v)) return nullptr;

        if(wantValue && !written.otherwise) {
            context.diagnostics.error("an `if` used as a value needs an `else`"_v, expr.source);
            return nullptr;
        }

        if(!wantValue) {
            auto thenBlock = builder.addBlock(StringId());
            auto elseBlock = builder.addBlock(StringId());
            auto join = builder.addBlock(StringId());

            builder.addBranch(expr.source, builder.resultOf(*condition, UseMode::Inspect),
                              *thenBlock, *elseBlock);

            builder.setBlock(*thenBlock);
            resolveStatement(builder, written.then);
            if(!builder.isTerminated()) builder.addJump(expr.source, *join, nullptr, 0);

            builder.setBlock(*elseBlock);
            if(written.otherwise) resolveStatement(builder, written.otherwise.unwrap());
            if(!builder.isTerminated()) builder.addJump(expr.source, *join, nullptr, 0);

            // A merge both arms left is unreachable and still needs a terminator. `finish` is what
            // gives it one, because whether anything entered it is a question about the finished
            // edges rather than about this statement.
            builder.setBlock(*join);
            return nullptr;
        }

        auto& otherwise = written.otherwise.unwrap();
        return branchValue(builder, expr.source, *condition, hint,
            [&](DeclPtr<Type> armHint) { return resolveValue(builder, written.then, armHint); },
            [&](DeclPtr<Type> armHint) { return resolveValue(builder, otherwise, armHint); });
    }

    /*
     * A block, as a sequence of statements whose last may answer.
     *
     * `wantValue` is the position's question. A `= expr` body's block answers with its last
     * expression; a `:` body's answers with a `return`, and asking a statement block for a value
     * would be asking the wrong question of it.
     */
    Node* resolveBlock(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue) {
        auto scope = bindings.size();
        Node* result = nullptr;

        auto statements = expr.multi;
        auto count = statements.size();
        Size index = 0;

        for(auto statement: statements.contents(parse)) {
            index++;

            // A statement after control left is unreachable, and resolving it would append to a
            // block that already ends. Stopping is what the milestone needs; a warning about the
            // code that was dropped is a diagnostic question rather than a representation one.
            if(builder.isTerminated()) break;

            if(wantValue && index == count) {
                result = resolveValue(builder, statement, hint);
                break;
            }

            resolveStatement(builder, statement);
        }

        bindings.resize(U32(scope));
        return result;
    }

    /*
     * Statements.
     */

    void resolveStatement(Builder& builder, ast::Expr& expr) {
        switch(expr.kind) {
            case ast::Expr::Multi:
                resolveBlock(builder, expr, nullptr, false);
                return;
            case ast::Expr::Nested:
                resolveStatement(builder, *parse[expr.nested]);
                return;
            case ast::Expr::Decl:
                resolveDeclaration(builder, expr);
                return;
            case ast::Expr::Assign:
                resolveAssign(builder, expr);
                return;
            case ast::Expr::If:
                resolveIf(builder, expr, nullptr, false);
                return;
            case ast::Expr::While:
                resolveWhile(builder, expr);
                return;
            case ast::Expr::Ret:
                resolveReturn(builder, expr);
                return;
            default:
                // An expression in statement position: resolved, and its value discarded. Discarding
                // an owned one is where a drop will be emitted, which is why this is a resolve and
                // not a refusal - the position exists and has a rule, and the rule is empty while
                // every type is trivial.
                resolveValue(builder, expr, nullptr);
                return;
        }
    }

    /*
     * `let x = e` and `let &x = e`.
     *
     * Storage and an initialization, not a binding to a computation. That is what makes the
     * representation place-based: `x` names a place from here on, a later `x = e` is an assignment
     * to the same place, and both of those are things the ownership rules have opinions about. A
     * `let` that named a node instead would have to rediscover the storage the first time anything
     * was written through it.
     */
    void resolveDeclaration(Builder& builder, ast::Expr& expr) {
        for(auto binding: expr.decl.contents(parse)) {
            if(binding.pat.kind != ast::Pat::Var) {
                unsupported(binding.pat.source, "destructuring a declaration"_v);
                continue;
            }

            if(binding.in || binding.alts.size()) {
                unsupported(binding.pat.source, "a declaration with a body"_v);
                continue;
            }

            if(!binding.content) {
                context.diagnostics.error("a local requires an initializer"_v, binding.pat.source);
                continue;
            }

            if(binding.bind == ast::BindType::Sink) {
                unsupported(binding.pat.source, "a `let ->` declaration"_v);
                continue;
            }

            auto value = resolveValue(builder, *parse[binding.content], nullptr);
            if(!value) return;

            auto local = builder.addLocal(binding.pat.var, owned(valueType(*value)),
                                          binding.pat.source, binding.bind == ast::BindType::Ref);

            auto place = builder.localPlace(*local);
            auto operand = builder.resultOf(*value, UseMode::Take);
            builder.addWrite(binding.pat.source, *place, operand, Destination::Init);

            // In scope from *after* its own initializer, which is what makes `let x = x` read the
            // outer `x` rather than itself.
            bindings.push(Binding { binding.pat.var, local });
        }
    }

    void resolveAssign(Builder& builder, ast::Expr& expr) {
        auto& written = *parse[expr.assign];

        if(written.target.kind != ast::Expr::Var) {
            unsupported(written.target.source, "assigning to anything but a name"_v);
            return;
        }

        auto place = assignablePlace(builder, written.target, written.target.var);
        if(!place) return;

        auto value = resolveValue(builder, written.value, place->occurrence.type);
        if(!value) return;
        if(!checkType(*value, place->occurrence.type, written.value.source, "an assigned value"_v)) return;

        auto operand = builder.resultOf(*value, UseMode::Take);
        builder.addWrite(expr.source, *place, operand, Destination::Assign);
    }

    // The place a name stands for, where the name is being written rather than read. Writability is
    // checked here rather than at the builder because this is where the source position is: the
    // builder asserts the same thing, and an assertion is not a diagnostic.
    Place* assignablePlace(Builder& builder, ast::Expr& target, StringId name) {
        Place* place = nullptr;

        if(auto local = findLocal(name)) {
            place = builder.localPlace(*local);
        } else if(auto found = program.findGlobal(&module - *program.decl, name)) {
            if(!decl()[found]->occurrence.type) return nullptr;
            place = builder.globalPlace(found);
        } else {
            context.diagnostics.error("unknown name %@"_v, target.source, context.findName(name));
            return nullptr;
        }

        if(!place->writable) {
            context.diagnostics.error("%@ is not declared `&`, so it cannot be assigned"_v,
                                      target.source, context.findName(name));
            return nullptr;
        }

        return place;
    }

    /*
     * `while c: body`.
     *
     * Three blocks and no parameters: the loop's state is the storage its locals already have, so
     * nothing has to be threaded around the back edge. That is a property of this milestone rather
     * than of the representation - a loop carrying an owned value will pass it as a block argument,
     * which is why the edge form exists before there is a loop that needs it.
     *
     * The condition is resolved *into* the header, which is what makes `while a && b:` correct: the
     * blocks `&&` builds sit between the header and the branch, and the back edge below returns to
     * the header rather than to wherever the condition ended.
     */
    void resolveWhile(Builder& builder, ast::Expr& expr) {
        auto& written = *parse[expr.whileLoop];

        auto header = builder.addBlock(StringId());
        auto body = builder.addBlock(StringId());
        auto exit = builder.addBlock(StringId());

        builder.addJump(expr.source, *header, nullptr, 0);

        builder.setBlock(*header);
        auto condition = resolveValue(builder, written.cond, program.types.boolean);

        // A condition that did not resolve leaves three blocks that still have to be closed: the
        // header has no branch to give, and the body is entered from nothing. Both are `unreachable`
        // and the walk continues at the exit, so one bad condition produces one diagnostic rather
        // than a cascade about the statements after the loop.
        if(!condition || !checkType(*condition, program.types.boolean, written.cond.source, "a condition"_v)) {
            if(!builder.isTerminated()) builder.addUnreachable(expr.source);
            builder.setBlock(*body);
            builder.addUnreachable(expr.source);
            builder.setBlock(*exit);
            return;
        }

        builder.addBranch(expr.source, builder.resultOf(*condition, UseMode::Inspect), *body, *exit);

        builder.setBlock(*body);
        auto scope = bindings.size();
        resolveStatement(builder, written.body);
        bindings.resize(U32(scope));

        if(!builder.isTerminated()) builder.addJump(expr.source, *header, nullptr, 0);

        builder.setBlock(*exit);
    }

    void resolveReturn(Builder& builder, ast::Expr& expr) {
        auto& function = *currentFunction;

        if(!expr.ret) {
            if(decl()[function.result.type]->kind != Type::Unit) {
                context.diagnostics.error("this function answers %@, so `return` needs a value"_v,
                                          expr.source, typeText(function.result.type));
                builder.addUnreachable(expr.source);
                return;
            }

            builder.addReturnUnit(expr.source);
            return;
        }

        auto value = resolveValue(builder, *parse[expr.ret], function.result.type);
        if(!value) {
            if(!builder.isTerminated()) builder.addUnreachable(expr.source);
            return;
        }

        if(!checkType(*value, function.result.type, expr.source, "a returned value"_v)) {
            builder.addUnreachable(expr.source);
            return;
        }

        builder.addReturn(expr.source, builder.resultOf(*value, UseMode::Take));
    }

    /*
     * Bodies.
     */

    // The function whose body is being resolved, which `return` needs and nothing else does.
    Function* currentFunction = nullptr;

    void resolveBody(Function& function, ast::Decl& declaration) {
        auto& written = declaration.fun;
        if(!written.body) return;

        Builder builder(context, program, function);
        currentFunction = &function;
        bindings.clear();

        /*
         * The parameters, as storage and as names, before anything in the body can read one.
         *
         * A parameter is a place from the first statement, which is what every later milestone needs
         * of it: a field of one is projected, a borrow of one has an extent, and `&x: T` writes
         * through it. Making it a value the body copies into a local would be the same storage
         * described twice, and the second description is the one a borrow would have to be checked
         * against.
         */
        for(auto argumentPointer: function.args.contents(decl())) {
            auto local = builder.addParameter(*decl()[argumentPointer]);
            bindings.push(Binding { local->name, local });
        }

        auto& expr = *parse[written.body];
        auto unit = decl()[function.result.type]->kind == Type::Unit;

        if(written.implicitReturn) {
            // `fn f(...) = expr` answers `expr`. A unit-returning one still evaluates it, and what
            // it returns is nothing rather than the value.
            auto value = resolveValue(builder, expr, unit ? nullptr : function.result.type);

            if(value && !builder.isTerminated()) {
                if(unit) {
                    builder.addReturnUnit(expr.source);
                } else if(checkType(*value, function.result.type, expr.source, "a returned value"_v)) {
                    builder.addReturn(expr.source, builder.resultOf(*value, UseMode::Take));
                }
            }
        } else {
            // `fn f(...): statements` answers through `return`, which is what the language says - a
            // block body's last statement is a statement and not a result.
            resolveStatement(builder, expr);
        }

        finish(builder, function, unit, declaration.source);
        builder.seal();
        currentFunction = nullptr;
    }

    /*
     * Every block that has no terminator, given one.
     *
     * Falling off the end of a unit-returning function is a return; falling off the end of any other
     * is an error the *function* is reported for, once, however many paths reach the end. Blocks a
     * diagnostic already abandoned are closed with `unreachable` rather than reported again, so one
     * mistake produces one message.
     */
    void finish(Builder& builder, Function& function, bool unit, LocationId source) {
        auto& body = *(*program.mir)[function.mir];
        auto base = *program.mir;
        auto reported = false;

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));
            auto count = block->nodes.size();

            if(count && isTerminator(base[block->nodes.get(base, count - 1)]->kind)) continue;

            builder.setBlock(*block);

            if(unit) {
                builder.addReturnUnit(source);
                continue;
            }

            // A block with no predecessors is one the arms of an `if` both left; it is unreachable
            // rather than a path that falls off the end, and reporting it would be reporting the
            // shape of the control flow rather than a mistake.
            if(i != 0 && !enteredFrom(body, base, *block)) {
                builder.addUnreachable(source);
                continue;
            }

            if(!reported && !context.diagnostics.errorCount()) {
                context.diagnostics.error("this function answers %@, and a path through it reaches the end without returning"_v,
                                          function.source, typeText(function.result.type));
                reported = true;
            }

            builder.addUnreachable(source);
        }
    }

    // Whether any terminator names this block. `Block::incoming` is `seal`'s answer and this runs
    // before it, so the question is asked of the edges themselves.
    bool enteredFrom(Body& body, MirBase base, Block& block) {
        auto target = &block - base;

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto other = body.block(base, BlockId(i));
            auto count = other->nodes.size();
            if(!count) continue;

            auto last = base[other->nodes.get(base, count - 1)];
            if(!isTerminator(last->kind)) continue;

            auto found = false;
            forEachEdge(*last, [&](Edge& edge) { if(edge.target == target) found = true; });
            if(found) return true;
        }

        return false;
    }

    /*
     * Pass two, and it is two walks rather than one.
     *
     * Every initializer before any body, because a `let`'s *type* is what its initializer says and a
     * body that reads the name needs it. Declaring the name in pass one is not enough for that: what
     * pass one records is that the module has a global called `answer`, and what a read of it needs
     * is what is in it. One interleaved walk resolved them in source order, so a function above the
     * `let` it read was resolved against a global with no type at all.
     *
     * The two counters are per file and per walk, for the reason `writtenGlobals` exists: they index
     * the *written* declarations including the rejected ones, so they have to advance in step with
     * the same traversal that filled the arrays.
     */
    void resolveInitializers(ast::Module& file) {
        Size next = 0;
        for(auto declaration: file.decls.contents(parse)) {
            if(declaration.kind == ast::Decl::Stmt) resolveGlobals(declaration, next);
        }
    }

    void resolveBodies(ast::Module& file) {
        Size next = 0;
        for(auto declaration: file.decls.contents(parse)) {
            if(declaration.kind != ast::Decl::Fun) continue;
            if(next >= writtenFunctions.size()) continue;

            // Null where the first pass rejected it - see `writtenFunctions`. Its body is not
            // resolved, because there is no signature to resolve it against and every name in it
            // would be reported a second time.
            auto pointer = writtenFunctions[next++];
            if(pointer) resolveBody(*decl()[pointer], declaration);
        }
    }
};

} // namespace

Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root) {
    auto program = Ptr<Program>(new Program());
    auto module = program->addModule(root.name);

    Resolver resolver { context, *program, *context.parseRegion, *module };

    for(auto file: root.files) resolver.declare(*file);
    for(auto file: root.files) resolver.resolveInitializers(*file);
    for(auto file: root.files) resolver.resolveBodies(*file);

    /*
     * Where the program starts.
     *
     * `main` where the module declares one. Once top-level statements are implemented this becomes a
     * synthesized entry that runs them and then calls `main`, for the reason the current compiler
     * has one: a program with initialization that started at `main` would start after its own
     * initialization had been skipped.
     */
    auto name = context.addUnqualifiedName("main", 4);
    auto modulePointer = module - *program->decl;
    if(auto found = program->findFunction(modulePointer, name)) program->entry = found;

    if(context.diagnostics.errorCount()) return program;

    /*
     * Verified before it is summarized, and summarized before anything reads a published fact.
     *
     * That order is not a convenience: `computeSummaries` walks every body's nodes and places to
     * build the call graph, so it is reading IR that has to be structurally sound first - a call
     * naming a callee that is not there would otherwise be found by the summary pass, which has no
     * source position to report it at.
     */
    if(!verifyProgram(context, *program)) return program;

    computeSummaries(context, *program);
    return program;
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root) {
    ast::ModuleGroup group { root.name };
    group.files.push(&root);
    return resolveProgram(context, group);
}

} // namespace mir
