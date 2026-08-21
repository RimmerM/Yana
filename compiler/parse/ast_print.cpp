#include "ast_print.h"

using namespace ast;

inline void write(Net::Writer& stream, const String& string) {
    stream.writeString(string);
}

template<class T>
inline void printValue(Net::Writer& writer, const T& i) {
    writer.writeBytes(128, [&](Byte* c) {
        return show(i, (char*)c, 128);
    });
}

struct Printer {
    Printer(Context& context, Net::Writer& stream, ParseBase base) : context(context), stream(stream), base(base) {}

    void toString(Expr& expr) {
        if(expr.kind >= Expr::Lit) {
            printLitExpr(expr);
            return;
        }

        switch(expr.kind) {
            case Expr::Error: stream.writeString("<parse error>"_v); break;
            case Expr::Multi: printMultiExpr(expr); break;
            case Expr::Lit: printLitExpr(expr); break;
            case Expr::Var: printVarExpr(expr); break;
            case Expr::App: printAppExpr(expr); break;
            case Expr::Sub: printSubExpr(expr); break;
            case Expr::Fun: printFunExpr(expr); break;
            case Expr::Infix: printInfixExpr(expr); break;
            case Expr::Prefix: printPrefixExpr(expr); break;
            case Expr::If: printIfExpr(expr); break;
            case Expr::MultiIf: printMultiIfExpr(expr); break;
            case Expr::Decl: printDeclExpr(expr); break;
            case Expr::While: printWhileExpr(expr); break;
            case Expr::For: printForExpr(expr); break;
            case Expr::Assign: printAssignExpr(expr); break;
            case Expr::Nested: printNestedExpr(expr); break;
            case Expr::Coerce: printCoerceExpr(expr); break;
            case Expr::Field: printFieldExpr(expr); break;
            case Expr::Con: printConExpr(expr); break;
            case Expr::Tup: printTupExpr(expr); break;
            case Expr::TupUpdate: printTupUpdateExpr(expr); break;
            case Expr::Array: printArrayExpr(expr); break;
            case Expr::Map: printMapExpr(expr); break;
            case Expr::Format: printFormatExpr(expr); break;
            case Expr::Range: printRangeExpr(expr); break;
            case Expr::Match: printMatchExpr(expr); break;
            case Expr::Ret: printRetExpr(expr); break;
            case Expr::Yield: printYieldExpr(expr); break;
            case Expr::Break: printBreakExpr(expr); break;
            case Expr::Continue: stream.writeString("ContinueExpr"_v); break;
            case Expr::Is: printIsExpr(expr); break;
            case Expr::Try: printTryExpr(expr); break;
            case Expr::Unwrap: printUnwrapExpr(expr); break;
        }
    }

    void toString(Decl& decl) {
        toString(&decl.attributes, "Decl"_v, [&] {
            if(decl.exported) stream.writeString("<pub> "_v);
            switch(decl.kind) {
                case Decl::Error: printErrorDecl(decl); break;
                case Decl::Fun: printFunDecl(decl); break;
                case Decl::Alias: printAliasDecl(decl); break;
                case Decl::Data: printDataDecl(decl); break;
                case Decl::Foreign: printForeignDecl(decl); break;
                case Decl::Stmt: printStmtDecl(decl); break;
                case Decl::Trait: printTraitDecl(decl); break;
                case Decl::Instance: printInstanceDecl(decl); break;
                case Decl::Attr: printAttrDecl(decl); break;
            }
        });
    }

    void toString(Import& import) {
        stream.writeString("Import "_v);

        if(import.qualified) {
            stream.writeString("<qualified> "_v);
        }

        write(stream, context.findName(import.from));
        stream.writeByte(' ');

        auto localName = context.find(import.localName);
        if(localName.textLength > 0) {
            stream.writeString("<as> "_v);
            stream.writeBytes((const Byte*)localName.text, localName.textLength);
        }

        if(import.exclude.isNotEmpty() || import.include.isNotEmpty()) {
            makeLevel();

            if(import.include.isNotEmpty()) {
                toStringIntro(import.exclude.isEmpty());
                stream.writeString("<include>"_v);

                makeLevel();
                auto v = import.include.contents(base);

                for(auto i = v.begin(); i != v.end(); ++i) {
                    toStringIntro(i == v.back());
                    stream.writeString("Symbol "_v);
                    write(stream, context.findName(*i));
                }

                removeLevel();
            }

            if(import.exclude.isNotEmpty()) {
                toStringIntro(true);
                stream.writeString("<hide>"_v);

                makeLevel();
                auto v = import.exclude.contents(base);

                for(auto i = v.begin(); i != v.end(); ++i) {
                    toStringIntro(i == v.back());
                    stream.writeString("Symbol "_v);
                    write(stream, context.findName(*i));
                }

                removeLevel();
            }

            removeLevel();
        }
    }

    void toString(Fixity& fixity) {
        stream.writeString("Fixity "_v);

        write(stream, context.findName(fixity.op));

        stream.writeByte(' ');
        stream.writeString(fixity.kind == Fixity::Left ? "infixl"_v : "infixr"_v);
        stream.writeByte(' ');
        printValue(stream, fixity.precedence);
    }

    void toString(Module& mod) {
        stream.writeString("Module "_v);

        auto imports = mod.imports.contents(base);
        if(imports.size() > 0) {
            makeLevel();
            for(auto i = imports.begin(); i != imports.end(); ++i) {
                auto it = *i;
                toString(it, i == imports.back());
            }
            removeLevel();
        }

        auto decls = mod.decls.contents(base);
        if(decls.size() > 0) {
            makeLevel();
            for(auto i = decls.begin(); i != decls.end(); ++i) {
                auto it = *i;
                toString(it, i == decls.back());
            }
            removeLevel();
        }

        auto ops = mod.ops.contents(base);
        if(ops.size() > 0) {
            makeLevel();
            for(auto i = ops.begin(); i != ops.end(); ++i) {
                auto it = *i;
                toString(it, i == ops.back());
            }
            removeLevel();
        }

        stream.writeByte('\n');
    }

private:
    void makeIndent(bool isLast) {
        char f, s;
        if(isLast) {
            f = '`';
            s = '-';
        } else {
            f = '|';
            s = '-';
        }

        indentStack[indentStart-2] = f;
        indentStack[indentStart-1] = s;
    }

    void makeLevel() {
        if(indentStart) {
            indentStack[indentStart-1] = ' ';
            if(indentStack[indentStart-2] == '`') indentStack[indentStart-2] = ' ';
        }
        indentStack[indentStart] = ' ';
        indentStack[indentStart+1] = ' ';
        indentStack[indentStart+2] = 0;
        indentStart += 2;
    }

    void removeLevel() {
        indentStart -= 2;
    }

    void printMultiExpr(Expr& e) {
        stream.writeString("MultiExpr "_v);
        makeLevel();
        printList(e.multi);
        removeLevel();
    }

    void printLitExpr(Expr& e) {
        stream.writeString("LitExpr "_v);
        printLiteral(e.lit, (Literal::Kind)(e.kind - Expr::Lit));
    }

    void printVarExpr(Expr& e) {
        stream.writeString("VarExpr "_v);
        write(stream, context.findName(e.var));
    };

    void printAppExpr(Expr& e) {
        auto app = base[e.app];

        stream.writeString("AppExpr "_v);
        makeLevel();
        toString(app->callee, app->args.isEmpty());
        printList(app->args);
        removeLevel();
    }

    void printSubExpr(Expr& e) {
        auto app = base[e.sub];

        stream.writeString("SubExpr "_v);
        makeLevel();
        toString(app->callee, app->args.isEmpty());
        printList(app->args);
        removeLevel();
    }

    void printInfixExpr(Expr& e) {
        auto infix = base[e.infix];

        stream.writeString("InfixExpr "_v);
        makeLevel();
        toString(infix->op, false);
        toString(infix->lhs,  false);
        toString(infix->rhs, true);
        removeLevel();
    }

    void printPrefixExpr(Expr& e) {
        auto prefix = base[e.prefix];

        stream.writeString("PrefixExpr "_v);
        makeLevel();
        toString(prefix->op, false);
        toString(prefix->on, true);
        removeLevel();
    }

    void printIfExpr(Expr& e) {
        auto ifExpr = base[e.singleIf];

        stream.writeString("IfExpr "_v);
        makeLevel();
        toString(ifExpr->cond, false);
        if(ifExpr->otherwise) {
            toString(ifExpr->then, false);
            toString(ifExpr->otherwise.unwrap(), true);
        } else {
            toString(ifExpr->then, true);
        }
        removeLevel();
    }

    void printMultiIfExpr(Expr& e) {
        stream.writeString("MultiIfExpr "_v);
        makeLevel();
        printList(e.multiIf);
        removeLevel();
    }

    void toString(VarDecl& e) {
        stream.writeString("VarDecl "_v);
        if(e.pat.kind == Pat::Var) {
            write(stream, context.findName(e.pat.var));
        }

        switch(e.bind) {
            case BindType::Borrow:
                stream.writeString(" <borrow> "_v);
                break;
            case BindType::Ref:
                stream.writeString(" <ref> "_v);
                break;
            case BindType::Sink:
                stream.writeString(" <sink> "_v);
                break;
        }

        makeLevel();

        if(e.attributes.isNotEmpty()) {
            toStringIntro(false);
            stream.writeString("<attributes>"_v);
            makeLevel();
            printList(e.attributes);
            removeLevel();
        }

        toString(e.pat, false);
        if(e.content) toString(*base[e.content], e.in == nullptr && e.alts.isEmpty());
        if(e.alts.isNotEmpty()) {
            toStringIntro(e.in == nullptr);
            stream.writeString("Else"_v);
            makeLevel();
            printList(e.alts);
            removeLevel();
        }

        if(e.in) {
            toStringIntro(true);
            stream.writeString("In"_v);
            makeLevel();
            toString(*base[e.in], true);
            removeLevel();
        }
        removeLevel();
    }

    void printDeclExpr(Expr& e) {
        stream.writeString("DeclExpr "_v);
        makeLevel();
        printList(e.decl);
        removeLevel();
    }

    void printWhileExpr(Expr& e) {
        auto w = base[e.whileLoop];

        stream.writeString("WhileExpr"_v);
        makeLevel();
        toString(w->cond, false);
        toString(w->body, true);
        removeLevel();
    }

    void printForExpr(Expr& e) {
        auto f = base[e.forLoop];

        stream.writeString("ForExpr"_v);

        if(f->reverse) {
            stream.writeString(" <reverse>"_v);
        }

        if(f->inclusive) {
            stream.writeString(" <inclusive>"_v);
        }

        makeLevel();
        toString(f->pat, false);
        toString(f->from, false);
        if(f->to) toString(*base[f->to], false);
        if(f->step) toString(*base[f->step], false);
        toString(f->body, true);
        removeLevel();
    }

    void printAssignExpr(Expr& e) {
        auto assign = base[e.assign];

        stream.writeString("AssignExpr "_v);
        makeLevel();
        toString(assign->target, false);
        toString(assign->value, true);
        removeLevel();
    }

    void printNestedExpr(Expr& e) {
        stream.writeString("NestedExpr "_v);
        makeLevel();
        toString(*base[e.nested], true);
        removeLevel();
    }

    void printCoerceExpr(Expr& e) {
        auto coerce = base[e.coerce];

        stream.writeString("CoerceExpr "_v);
        makeLevel();
        toString(coerce->type, false);
        toString(coerce->target, true);
        removeLevel();
    }

    void printFieldExpr(Expr& e) {
        auto field = base[e.field];

        stream.writeString("FieldExpr "_v);
        makeLevel();
        toString(field->field, false);
        toString(field->target, true);
        removeLevel();
    }

    void printRangeExpr(Expr& e) {
        auto range = base[e.range];

        stream.writeString("RangeExpr "_v);
        if(range->reverse) stream.writeString("<reverse> "_v);

        makeLevel();
        toString(range->from, false);
        toString(range->to, true);
        removeLevel();
    }

    void printConExpr(Expr& e) {
        auto con = base[e.con];

        stream.writeString("ConExpr "_v);
        makeLevel();
        toString(con->type, false);
        printList(con->args);
        removeLevel();
    }

    void printTupExpr(Expr& e) {
        stream.writeString("TupExpr"_v);
        makeLevel();
        printList(e.tup);
        removeLevel();
    }

    void printTupUpdateExpr(Expr& e) {
        auto tup = base[e.tupUpdate];

        stream.writeString("TupUpdateExpr "_v);
        if(tup->bind == BindType::Sink) stream.writeString("<sink> "_v);

        makeLevel();
        toString(tup->value, false);
        printList(tup->args);
        removeLevel();
    }

    void printArrayExpr(Expr& e) {
        stream.writeString("ArrayExpr "_v);
        makeLevel();
        printList(e.arr);
        removeLevel();
    }

    void printMapExpr(Expr& e) {
        stream.writeString("MapExpr "_v);
        makeLevel();
        printList(e.map);
        removeLevel();
    }

    void printFormatExpr(Expr& e) {
        stream.writeString("FormatExpr "_v);
        makeLevel();
        printList(e.format);
        removeLevel();
    }

    void printMatchExpr(Expr& e) {
        auto match = base[e.match];

        stream.writeString("MatchExpr "_v);
        makeLevel();
        toString(match->pivot, match->alts.isEmpty());
        printList(match->alts);
        removeLevel();
    }

    void printIsExpr(Expr& e) {
        auto test = base[e.is];

        stream.writeString("IsExpr "_v);
        makeLevel();
        toString(test->value, false);
        toString(test->pat, true);
        removeLevel();
    }

    void printUnwrapExpr(Expr& e) {
        stream.writeString("UnwrapExpr "_v);
        makeLevel();
        toString(*base[e.unwrap], true);
        removeLevel();
    }

    void printTryExpr(Expr& e) {
        stream.writeString("TryExpr "_v);
        makeLevel();
        toString(*base[e.tryValue], true);
        removeLevel();
    }

    void printRetExpr(Expr& e) {
        stream.writeString("RetExpr "_v);
        if(e.ret) {
            makeLevel();
            toString(*base[e.ret], true);
            removeLevel();
        }
    }

    void printYieldExpr(Expr& e) {
        stream.writeString("YieldExpr "_v);
        if(e.yield) {
            makeLevel();
            toString(*base[e.yield], true);
            removeLevel();
        }
    }

    void printBreakExpr(Expr& e) {
        stream.writeString("BreakExpr "_v);
        if(e.breakValue) {
            makeLevel();
            toString(*base[e.breakValue], true);
            removeLevel();
        }
    }

    // Writes just the bare tag ("<lens>"/"<iter>", nothing for Plain) - callers are
    // responsible for surrounding spacing, since it differs slightly by call site.
    void printFunKind(FunKind kind) {
        switch(kind) {
            case FunKind::Plain: break;
            case FunKind::Lens: stream.writeString("<lens>"_v); break;
            case FunKind::Iter: stream.writeString("<iter>"_v); break;
        }
    }

    // The markers and the binding convention are written in source order, which is also the order
    // they are parsed in: `@lazy return &value: T`.
    void printArgConvention(bool returnRoot, BindType bind, bool lazy = false) {
        if(lazy) stream.writeString("@lazy "_v);
        if(returnRoot) stream.writeString("return "_v);

        switch(bind) {
            case BindType::Borrow: break;
            case BindType::Ref: stream.writeString("&"_v); break;
            case BindType::Sink: stream.writeString("->"_v); break;
        }
    }

    void printFunExpr(Expr& e) {
        auto f = base[e.fun];
        stream.writeString("FunExpr "_v);
        if(f->kind != FunKind::Plain) {
            printFunKind(f->kind);
            stream.writeByte(' ');
        }
        stream.writeByte('(');

        auto contents = f->args.contents(base);
        for(auto a = contents.begin(); a != contents.end(); ++a) {
            printArgConvention((*a).returnRoot, (*a).bind, (*a).lazy);
            write(stream, context.findName((*a).name));

            if((*a).type) {
                stream.writeString(": "_v);
                toString(*base[(*a).type]);
            }

            if(a != contents.back()) stream.writeString(", "_v);
        }

        stream.writeByte(')');

        makeLevel();
        toString(f->body, true);
        removeLevel();
    }

    void toString(Alt& alt) {
        stream.writeString("Alt"_v);

        makeLevel();
        toString(alt.pat, false);
        toString(alt.expr, true);
        removeLevel();
    }

    // A declaration that did not parse, with the name it got as far as reading where there was one.
    void printErrorDecl(Decl& e) {
        stream.writeString("<parse error>"_v);

        if(e.errorName) {
            stream.writeByte(' ');
            write(stream, context.findName(e.errorName));
        }
    }

    void printFunDecl(Decl& e) {
        stream.writeString("FunDecl "_v);
        write(stream, context.findName(e.fun.name));

        if(e.fun.kind != FunKind::Plain) {
            stream.writeByte(' ');
            printFunKind(e.fun.kind);
        }

        if(e.fun.implicitReturn) {
            stream.writeString(" <implicit return> "_v);
        }

        makeLevel();
        toString(e.fun.constraints, e.fun.args.isEmpty() && !e.fun.ret && !e.fun.body);

        if(e.fun.args.isNotEmpty()) {
            auto args = e.fun.args.contents(base);
            for(auto i = args.begin(); i != args.end(); ++i) {
                auto arg = *i;
                toStringIntro(i == args.back() && e.fun.ret == nullptr && e.fun.body == nullptr);

                stream.writeString("Arg "_v);
                printArgConvention(arg.returnRoot, arg.bind, arg.lazy);
                write(stream, context.findName(arg.name));

                makeLevel();
                if(arg.type) toString(*base[arg.type], arg.def == nullptr);
                if(arg.def) toString(*base[arg.def], true);
                removeLevel();
            }
        }

        if(e.fun.ret) {
            toStringIntro(e.fun.body == nullptr);
            stream.writeString("Result"_v);
            makeLevel();
            toString(*base[e.fun.ret], true);
            removeLevel();
        }

        if(e.fun.body) {
            toStringIntro(true);
            stream.writeString("Body"_v);
            makeLevel();
            toString(*base[e.fun.body], true);
            removeLevel();
        }
        removeLevel();
    }

    void printAliasDecl(Decl& e) {
        stream.writeString(e.qualified ? "AliasDecl <qualified> "_v : "AliasDecl "_v);
        toString(e.alias.type);
        makeLevel();
        toString(e.alias.target, true);
        removeLevel();
    }

    void printDataDecl(Decl& e) {
        stream.writeString("DataDecl "_v);
        toString(e.data.type);
        makeLevel();
        toString(e.data.constraints, false);
        printList(e.data.cons);
        removeLevel();
    }

    void printForeignDecl(Decl& e) {
        stream.writeString("ForeignDecl "_v);
        auto name = context.find(e.foreign.externName);
        stream.writeBytes((const Byte*)name.text, name.textLength);
        stream.writeByte(' ');

        auto localName = context.find(e.foreign.localName);
        if(localName.textLength > 0) {
            stream.writeBytes((const Byte*)localName.text, localName.textLength);
        } else {
            stream.writeBytes((const Byte*)name.text, name.textLength);
        }

        makeLevel();
        toString(e.foreign.type, true);
        removeLevel();
    }

    void printStmtDecl(Decl& e) {
        stream.writeString("StmtDecl"_v);
        makeLevel();
        toString(e.stmt, true);
        removeLevel();
    }

    void printTraitDecl(Decl& e) {
        stream.writeString("TraitDecl "_v);
        toString(e.trait.type);

        makeLevel();
        toString(e.trait.constraints, e.trait.decls.isEmpty());
        printList(e.trait.decls);
        removeLevel();
    }

    void printInstanceDecl(Decl& e) {
        stream.writeString("InstanceDecl "_v);
        makeLevel();
        toString(e.instance.type, e.instance.constraints.isEmpty() && e.instance.decls.isEmpty());
        toString(e.instance.constraints, e.instance.decls.isEmpty());
        printList(e.instance.decls);
        removeLevel();
    }

    void printAttrDecl(Decl& e) {
        stream.writeString("AttrDecl "_v);
        write(stream, context.findName(e.attr.name));

        makeLevel();
        toString(e.attr.type, true);
        removeLevel();
    }

    void toString(FormatChunk& f, bool last) {
        auto name = context.find(f.string);
        if(f.format) {
            toString(*base[f.format], name.textLength > 0 ? false : last);
        }

        if(name.textLength > 0) {
            toStringIntro(last);
            stream.writeString("LitExpr \""_v);
            stream.writeBytes((const Byte*)name.text, name.textLength);
            stream.writeByte('"');
        }
    }

    void toString(IfCase& c) {
        stream.writeString("IfCase "_v);

        makeLevel();
        toString(c.cond, false);
        toString(c.then, true);
        removeLevel();
    }

    void toString(TupArg& arg) {
        stream.writeString("Field "_v);

        auto name = context.find(arg.name);
        if(name.textLength > 0) {
            stream.writeBytes((const Byte*)name.text, name.textLength);
        } else {
            stream.writeString("<anonymous>"_v);
        }

        makeLevel();
        toString(arg.value, true);
        removeLevel();
    }

    void toString(TupUpdateArg& arg) {
        stream.writeString("Field "_v);

        auto path = arg.path.contents(base);
        for(auto i = path.begin(); i != path.end(); ++i) {
            write(stream, context.findName(*i));
            if(i != path.back()) stream.writeByte('.');
        }

        makeLevel();
        toString(arg.value, true);
        removeLevel();
    }

    void toString(MapArg& arg) {
        stream.writeString("Entry "_v);

        makeLevel();
        toString(arg.key, false);
        toString(arg.value, true);
        removeLevel();
    }

    void toString(SimpleType& t) {
        auto name = context.find(t.name);
        if(name.textLength > 0) {
            stream.writeBytes((const Byte*)name.text, name.textLength);
            stream.writeByte(' ');
        }

        if(t.kind.isNotEmpty()) {
            stream.writeByte('(');
            auto k = t.kind.contents(base);
            U16 index = 0;

            for(auto i = k.begin(); i != k.end(); ++i) {
                // The functional dependency's arrow stands where a separator would, so it prints
                // as one: `Contiguous(c -> a)` reads back as what was written.
                if(index && index == t.determined) stream.writeString(" -> "_v);
                else if(index) stream.writeString(", "_v);

                write(stream, context.findName((*i).name));

                /*
                 * The annotation, which is what makes this a const parameter rather than a type one.
                 *
                 * Its name, because §2.5 admits only the integer types and a named type is what
                 * every one of them is. A head is printed on one line and a type is printed as a
                 * subtree, so anything wider than a name has no room here - and a head that needed
                 * one would be a head this printer should be told about rather than guess at.
                 */
                if((*i).type) {
                    auto& annotation = *base[(*i).type];
                    stream.writeString(": "_v);

                    if(annotation.kind == Type::Con || annotation.kind == Type::Gen) {
                        write(stream, context.findName(annotation.name));
                    } else {
                        stream.writeString("<type>"_v);
                    }
                }

                // And the default, on the same terms and for the same reason: a name or a number is
                // what a default is, and both fit on the line the head is printed on.
                if((*i).def) {
                    auto& def = *base[(*i).def];
                    stream.writeString(" = "_v);

                    if(def.kind == Type::Con || def.kind == Type::Gen) {
                        write(stream, context.findName(def.name));
                    } else if(def.kind == Type::Lit && base[def.lit]) {
                        printValue(stream, (I64)base[def.lit]->lit.i());
                    } else {
                        stream.writeString("<type>"_v);
                    }
                }

                index++;
            }

            stream.writeByte(')');
        }
    }

    void toString(Con& c) {
        auto name = context.find(c.name);
        if(name.textLength > 0) {
            stream.writeString("Constructor "_v);
            stream.writeBytes((const Byte*)name.text, name.textLength);

            makeLevel();
            if(c.attributes.isNotEmpty()) {
                toStringIntro(c.content == nullptr);
                stream.writeString("<attributes>"_v);
                makeLevel();
                printList(c.attributes);
                removeLevel();
            }

            if(c.content) {
                toString(*base[c.content], true);
            }

            removeLevel();
        } else {
            stream.writeString("<invalid name>"_v);
        }
    }

    void toString(Attribute& attribute) {
        stream.writeString("Attribute "_v);
        auto name = context.find(attribute.name);
        if(name.textLength > 0) {
            stream.writeBytes((const Byte*)name.text, name.textLength);
        }

        if(attribute.args.isNotEmpty()) {
            makeLevel();
            printList(attribute.args);
            removeLevel();
        }
    }

    void toStringIntro(bool last) {
        stream.writeByte('\n');
        makeIndent(last);
        stream.writeBytes((const Byte*)indentStack, indentStart);
    }

    template<class T>
    void toString(T&& t, bool last) {
        toStringIntro(last);
        toString(t);
    }

    void printLiteral(const Literal& literal, Literal::Kind kind) {
        switch(kind) {
            case Literal::Int:
                printValue(stream, (I64)literal.i());
                break;
            case Literal::Double:
                printValue(stream, literal.d());
                break;
            case Literal::Float:
                printValue(stream, literal.f);
                break;
            case Literal::Char:
                printValue(stream, U32(literal.c));
                break;
            case Literal::String: {
                stream.writeByte('"');
                write(stream, context.findName(literal.s));
                stream.writeByte('"');
                break;
            }
            case Literal::Bool:
                if(literal.b) stream.writeString("True"_v);
                else stream.writeString("False"_v);
                break;
        }
    }

    void toString(Type& type) {
        toString(type.attributes ? base[type.attributes] : nullptr, "Type"_v, [&] {
            switch(type.kind) {
                case Type::Error:
                    stream.writeString("<parse error>"_v);
                    break;
                case Type::Unit:
                    stream.writeString("UnitType"_v);
                    break;
                case Type::Con: {
                    auto name = context.find(type.name);
                    stream.writeString("ConType "_v);
                    stream.writeBytes((const Byte*)name.text, name.textLength);
                    break;
                }
                case Type::Ptr:
                    stream.writeString("PtrType "_v);
                    makeLevel();
                    toString(*base[type.to], true);
                    removeLevel();
                    break;
                case Type::Ref:
                    stream.writeString("RefType "_v);
                    makeLevel();
                    toString(*base[type.to], true);
                    removeLevel();
                    break;
                case Type::Borrow:
                    stream.writeString("BorrowType "_v);
                    makeLevel();
                    toString(*base[type.to], true);
                    removeLevel();
                    break;
                case Type::Gen: {
                    stream.writeString("GenType "_v);
                    auto name = context.find(type.name);
                    stream.writeBytes((const Byte*)name.text, name.textLength);
                    break;
                }
                case Type::Tup:
                    printTupType(type);
                    break;
                case Type::Fun:
                    printFunType(type);
                    break;
                case Type::App:
                    printAppType(type);
                    break;
                case Type::Arr:
                    stream.writeString("ArrType "_v);
                    makeLevel();
                    toString(*base[type.arr.type], type.arr.length == nullptr);
                    if(type.arr.length) toString(*base[type.arr.length], true);
                    removeLevel();
                    break;
                case Type::Lit:
                    // A number written where a type is - `Vec(Float, 4)`. See ast::Type::Lit.
                    stream.writeString("LitType "_v);
                    makeLevel();
                    toString(*base[type.lit], true);
                    removeLevel();
                    break;
                case Type::Map:
                    stream.writeString("MapType "_v);
                    makeLevel();
                    toString(*base[type.map.from], false);
                    toString(*base[type.map.to], true);
                    removeLevel();
                    break;
            }
        });
    }

    void toString(TupField& field) {
        stream.writeString("Field "_v);
        if(field.name) {
            write(stream, context.findName(field.name));
        } else {
            stream.writeString("<anonymous>"_v);
        }

        makeLevel();
        toString(field.type, field.def == nullptr);
        if(field.def) toString(*base[field.def], true);
        removeLevel();
    }

    void printTupType(Type& type) {
        stream.writeString("TupType "_v);
        makeLevel();
        printList(type.tup.fields);
        removeLevel();
    }

    void toString(const ArgDecl& arg) {
        stream.writeString("Arg "_v);
        printArgConvention(arg.returnRoot, arg.bind, arg.lazy);

        auto name = context.find(arg.name);
        if(name.textLength > 0) {
            stream.writeBytes((const Byte*)name.text, name.textLength);
        } else {
            stream.writeString("<anonymous>"_v);
        }

        makeLevel();
        auto type = arg.type;
        toString(type, true);
        removeLevel();
    }

    void printFunType(Type& type) {
        auto fun = base[type.fun];

        stream.writeString("FunType "_v);
        if(fun->kind != FunKind::Plain) {
            printFunKind(fun->kind);
            stream.writeByte(' ');
        }
        makeLevel();

        auto contents = fun->args.contents(base);
        for(auto a = contents.begin(); a != contents.end(); ++a) {
            toString(*a, false);
        }

        toString(fun->ret, true);
        removeLevel();
    }

    void printAppType(Type& type) {
        auto app = base[type.app];

        stream.writeString("AppType "_v);
        makeLevel();
        toString(app->base, app->args.isEmpty());
        printList(app->args);
        removeLevel();
    }

    void toString(Pat& pat) {
        // `a@pat` binds the whole value as well as matching it against `pat`. A rest pattern
        // keeps its own name in the same field, and prints it as its content instead.
        if(pat.asVar && pat.kind != Pat::Rest) {
            write(stream, context.findName(pat.asVar));
            stream.writeByte('@');
        }

        if(pat.kind >= Pat::Lit) {
            stream.writeString("LitPat "_v);

            // The sign is the pattern's and the magnitude is the literal's - see Pat::negative - so
            // printing the literal alone would print `-1` as `1`. A dump says what was written.
            if(pat.negative) stream.writeByte('-');

            printLiteral(pat.lit, (Literal::Kind)(pat.kind - Pat::Lit));
            return;
        }

        switch(pat.kind) {
            // Handled above: a literal pattern's kind is `Lit` plus the literal's own, so every one
            // of them is greater than or equal to this and none reaches here.
            case Pat::Lit:
                break;
            case Pat::Error:
                stream.writeString("<parse error>"_v);
                break;
            case Pat::Var: {
                stream.writeString("VarPat "_v);
                auto name = context.find(pat.var);
                stream.writeBytes((const Byte*)name.text, name.textLength);
                break;
            }
            case Pat::Any:
                stream.writeString("AnyPat"_v);
                break;
            case Pat::Tup: {
                stream.writeString("TupPat"_v);
                makeLevel();

                auto fields = pat.tup.contents(base);
                for(auto i = fields.begin(); i != fields.end(); ++i) {
                    toStringIntro(i == fields.back());
                    stream.writeString("Field "_v);
                    auto name = context.find((*i).field);
                    if(name.textLength > 0) {
                        stream.writeBytes((const Byte*)name.text, name.textLength);
                    }

                    if((*i).pat) {
                        makeLevel();
                        toString(*base[(*i).pat], true);
                        removeLevel();
                    }
                }

                removeLevel();
                break;
            }
            case Pat::Con: {
                stream.writeString("ConPat "_v);
                write(stream, context.findName(pat.con.name));

                if(pat.con.pats) {
                    makeLevel();
                    toString(*base[pat.con.pats], true);
                    removeLevel();
                }
                break;
            }
            case Pat::Arr: {
                stream.writeString("ArrayPat "_v);
                makeLevel();
                printList(pat.arr);
                removeLevel();
                break;
            }
            case Pat::Rest: {
                stream.writeString("RestPat "_v);
                write(stream, context.findName(pat.asVar));
                break;
            }
            case Pat::Range: {
                stream.writeString("RangePat "_v);
                if(pat.range.inclusive) stream.writeString("<inclusive> "_v);

                makeLevel();
                toString(*base[pat.range.from], false);
                toString(*base[pat.range.to], true);
                removeLevel();
                break;
            }
            case Pat::Section: {
                stream.writeString("SectionPat "_v);
                write(stream, context.findName(pat.section.op));

                makeLevel();
                toString(*base[pat.section.bound], true);
                removeLevel();
                break;
            }
        }
    }

    void toString(Constraint& constraint) {
        switch(constraint.kind) {
            case Constraint::Error:
                stream.writeString("<parse error>"_v);
                break;
            case Constraint::Any: {
                stream.writeString("AnyConstraint "_v);
                auto name = context.find(constraint.name);
                stream.writeBytes((const Byte*)name.text, name.textLength);
                break;
            }
            case Constraint::Class: {
                stream.writeString("ClassConstraint "_v);
                write(stream, context.findName(constraint.klass.name));

                // The arguments are whole types now (§10.2), so they print as the subtree every
                // other written type in this dump does rather than as names on the head's line.
                if(constraint.klass.args.isNotEmpty()) {
                    makeLevel();
                    auto args = constraint.klass.args;
                    Size index = 0;

                    for(auto arg: args.contents(base)) {
                        toString(arg, ++index == args.size());
                    }

                    removeLevel();
                }
                break;
            }
            case Constraint::Field: {
                stream.writeString("FieldConstraint "_v);

                write(stream, context.findName(constraint.field.typeName));
                stream.writeByte('.');
                write(stream, context.findName(constraint.field.fieldName));

                makeLevel();
                toString(*base[constraint.field.type], true);
                removeLevel();
                break;
            }
            case Constraint::Function: {
                stream.writeString("FunctionConstraint "_v);
                write(stream, context.findName(constraint.fun.name));

                makeLevel();
                toString(*base[constraint.fun.type], true);
                removeLevel();
                break;
            }
            case Constraint::Const: {
                stream.writeString("ConstConstraint "_v);
                write(stream, context.findName(constraint.constant.name));

                makeLevel();
                toString(*base[constraint.constant.type], !constraint.constant.def);
                if(constraint.constant.def) toString(*base[constraint.constant.def], true);
                removeLevel();
                break;
            }
        }
    }

    void toString(ParseList<Constraint>& constraints, bool last) {
        if(constraints.isNotEmpty()) {
            toStringIntro(last);
            stream.writeString("<constraints>"_v);
            makeLevel();
            printList(constraints);
            removeLevel();
        }
    }

    template<class T>
    void printList(ParseList<T>& list) {
        auto contents = list.contents(base);
        for(auto a = contents.begin(); a != contents.end(); ++a) {
            auto it = *a;
            toString(it, a == contents.back());
        }
    }

    template<class F>
    void toString(ParseList<Attribute>* attributes, const StringView& name, F&& f) {
        if(attributes && attributes->isNotEmpty()) {
            stream.writeString(name);
            makeLevel();
            toStringIntro(false);

            stream.writeString("<attributes>"_v);
            makeLevel();
            printList(*attributes);
            removeLevel();

            toStringIntro(true);
            f();
            removeLevel();
        } else {
            f();
        }
    }

    Context& context;
    Net::Writer& stream;
    ParseBase base;

    U32 indentStart = 0;
    char indentStack[1024];
};

void printModule(Net::Writer& stream, Context& context, ParseBase base, Module& module) {
    Printer { context, stream, base }.toString(module);
    stream.flush();
}

void printDecl(Net::Writer& stream, Context& context, ParseBase base, Decl& decl) {
    Printer { context, stream, base }.toString(decl);
    stream.flush();
}

void printExpr(Net::Writer& stream, Context& context, ParseBase base, Expr& expr) {
    Printer { context, stream, base }.toString(expr);
    stream.flush();
}
