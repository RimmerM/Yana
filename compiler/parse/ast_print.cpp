#include "ast_print.h"

using namespace ast;

inline void write(std::ostream& stream, const String& string) {
    stream.write(string.text(), string.size());
}

struct Printer {
    Printer(Context& context, std::ostream& stream) : context(context), stream(stream) {}

    void toString(const Expr& expr) {
        switch(expr.type) {
            case Expr::Error: stream << "<parse error>"; break;
            case Expr::Multi: toString((const MultiExpr&)expr); break;
            case Expr::Lit: toString((const LitExpr&)expr); break;
            case Expr::Var: toString((const VarExpr&)expr); break;
            case Expr::App: toString((const AppExpr&)expr); break;
            case Expr::Fun: toString((const FunExpr&)expr); break;
            case Expr::Infix: toString((const InfixExpr&)expr); break;
            case Expr::Prefix: toString((const PrefixExpr&)expr); break;
            case Expr::If: toString((const IfExpr&)expr); break;
            case Expr::MultiIf: toString((const MultiIfExpr&)expr); break;
            case Expr::Decl: toString((const DeclExpr&)expr); break;
            case Expr::While: toString((const WhileExpr&)expr); break;
            case Expr::For: toString((const ForExpr&)expr); break;
            case Expr::Assign: toString((const AssignExpr&)expr); break;
            case Expr::Nested: toString((const NestedExpr&)expr); break;
            case Expr::Coerce: toString((const CoerceExpr&)expr); break;
            case Expr::Field: toString((const FieldExpr&)expr); break;
            case Expr::Con: toString((const ConExpr&)expr); break;
            case Expr::Tup: toString((const TupExpr&)expr); break;
            case Expr::TupUpdate: toString((const TupUpdateExpr&)expr); break;
            case Expr::Array: toString((const ArrayExpr&)expr); break;
            case Expr::Map: toString((const MapExpr&)expr); break;
            case Expr::Format: toString((const FormatExpr&)expr); break;
            case Expr::Case: toString((const CaseExpr&)expr); break;
            case Expr::Ret: toString((const RetExpr&)expr); break;
        }
    }

    void toString(const Decl& decl) {
        toString(decl.attributes, "Decl", [&] {
            switch(decl.kind) {
                case Decl::Error: stream << "<parse error>"; break;
                case Decl::Fun: toString((const FunDecl&)decl); break;
                case Decl::Alias: toString((const AliasDecl&)decl); break;
                case Decl::Data: toString((const DataDecl&)decl); break;
                case Decl::Foreign: toString((const ForeignDecl&)decl); break;
                case Decl::Stmt: toString((const StmtDecl&)decl); break;
                case Decl::Class: toString((const ClassDecl&)decl); break;
                case Decl::Instance: toString((const InstanceDecl&)decl); break;
                case Decl::Attr: toString((const AttrDecl&)decl); break;
            }
        });
    }

    void toString(const Import& import) {
        stream << "Import ";

        if(import.qualified) {
            stream << "<qualified> ";
        }

        write(stream, context.findName(import.from));
        stream << ' ';

        auto localName = context.find(import.localName);
        if(localName.textLength > 0) {
            stream << "<as> ";
            stream.write(localName.text, localName.textLength);
        }

        if(import.exclude || import.include) {
            makeLevel();

            if(import.include) {
                toStringIntro(import.exclude == nullptr);
                stream << "<include>";

                makeLevel();
                auto v = import.include;
                while(v) {
                    toStringIntro(v->next == nullptr);
                    stream << "Symbol ";
                    write(stream, context.findName(v->item));
                    v = v->next;
                }

                removeLevel();
            }

            if(import.exclude) {
                toStringIntro(true);
                stream << "<hide>";

                makeLevel();
                auto v = import.exclude;
                while(v) {
                    toStringIntro(v->next == nullptr);
                    stream << "Symbol ";
                    write(stream, context.findName(v->item));
                    v = v->next;
                }

                removeLevel();
            }

            removeLevel();
        }
    }

    void toString(const Fixity& fixity) {
        stream << "Fixity ";

        write(stream, context.findName(fixity.op));

        stream << ' ';
        stream << (fixity.kind == Fixity::Left ? "infixl" : "infixr");
        stream << ' ';
        stream << fixity.precedence;
    }

    void toString(const Module& mod) {
        stream << "Module ";
        Size max = mod.imports.size();
        if(max) {
            makeLevel();
            for(Size i = 0; i < max - 1; i++) {
                toString(mod.imports[i], false);
            }
            toString(mod.imports[max-1], true);
            removeLevel();
        }

        max = mod.decls.size();
        if(max) {
            makeLevel();
            for(Size i = 0; i < max - 1; i++) {
                toString(*mod.decls[i], false);
            }
            toString(*mod.decls[max-1], true);
            removeLevel();
        }

        max = mod.ops.size();
        if(max) {
            makeLevel();
            for(Size i = 0; i < max - 1; i++) {
                toString(mod.ops[i], false);
            }
            toString(mod.ops[max-1], true);
            removeLevel();
        }

        stream << '\n';
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

    void toString(const MultiExpr& e) {
        stream << "MultiExpr ";
        makeLevel();
        auto expr = e.exprs;
        while(expr) {
            toString(*expr->item, expr->next == nullptr);
            expr = expr->next;
        }
        removeLevel();
    }

    void toString(const LitExpr& e) {
        stream << "LitExpr ";
        toString(e.literal);
    }

    void toString(const VarExpr& e) {
        stream << "VarExpr ";
        write(stream, context.findName(e.name));
    };

    void toString(const AppExpr& e) {
        stream << "AppExpr ";
        makeLevel();
        toString(*e.callee, e.args == nullptr);
        auto arg = e.args;
        while(arg) {
            toString(arg->item, arg->next == nullptr);
            arg = arg->next;
        }
        removeLevel();
    }

    void toString(const InfixExpr& e) {
        stream << "InfixExpr ";
        write(stream, context.findName(e.op->name));
        makeLevel();
        toString(*e.lhs,  false);
        toString(*e.rhs, true);
        removeLevel();
    }

    void toString(const PrefixExpr& e) {
        stream << "PrefixExpr ";
        write(stream, context.findName(e.op->name));
        makeLevel();
        toString(*e.dst, true);
        removeLevel();
    }

    void toString(const IfExpr& e) {
        stream << "IfExpr ";
        makeLevel();
        toString(*e.cond, false);
        if(e.otherwise) {
            toString(*e.then, false);
            toString(*e.otherwise, true);
        } else {
            toString(*e.then, true);
        }
        removeLevel();
    }

    void toString(const MultiIfExpr& e) {
        stream << "MultiIfExpr ";
        makeLevel();
        auto a = e.cases;
        while(a) {
            toString(a->item, a->next == nullptr);
            a = a->next;
        }
        removeLevel();
    }

    void toString(const VarDecl& e) {
        stream << "VarDecl ";
        if(e.pat->kind == Pat::Var) {
            write(stream, context.findName(((const VarPat*)e.pat)->var));
        }

        switch(e.mut) {
            case VarDecl::Immutable:
                stream << " <const> ";
                break;
            case VarDecl::Ref:
                stream << " <ref> ";
                break;
            case VarDecl::Val:
                stream << " <flatten> ";
                break;
        }

        makeLevel();
        toString(*e.pat, false);
        toString(*e.content, e.in == nullptr && e.alts == nullptr);
        if(e.alts) {
            toStringIntro(e.in == nullptr);
            stream << "Else";
            makeLevel();
            auto a = e.alts;
            while(a) {
                toString(a->item, a->next == nullptr);
                a = a->next;
            }
            removeLevel();
        }

        if(e.in) {
            toStringIntro(true);
            stream << "In";
            makeLevel();
            toString(*e.in, true);
            removeLevel();
        }
        removeLevel();
    }

    void toString(const DeclExpr& e) {
        stream << "DeclExpr ";
        makeLevel();
        auto decl = e.decls;
        while(decl) {
            toString(decl->item, decl->next == nullptr);
            decl = decl->next;
        }
        removeLevel();
    }

    void toString(const WhileExpr& e) {
        stream << "WhileExpr";
        makeLevel();
        toString(*e.cond, false);
        toString(*e.loop, true);
        removeLevel();
    }

    void toString(const ForExpr& e) {
        stream << "ForExpr ";
        write(stream, context.findName(e.var));

        if(e.reverse) {
            stream << " <reverse>";
        }

        makeLevel();
        toString(*e.from, false);
        toString(*e.to, false);
        if(e.step) toString(*e.step, false);
        toString(*e.body, true);
        removeLevel();
    }

    void toString(const AssignExpr& e) {
        stream << "AssignExpr ";
        makeLevel();
        toString(*e.target, false);
        toString(*e.value, true);
        removeLevel();
    }

    void toString(const NestedExpr& e) {
        stream << "NestedExpr ";
        makeLevel();
        toString(*e.expr, true);
        removeLevel();
    }

    void toString(const CoerceExpr& e) {
        stream << "CoerceExpr ";
        makeLevel();
        toString(*e.kind, false);
        toString(*e.target, true);
        removeLevel();
    }

    void toString(const FieldExpr& e) {
        stream << "FieldExpr ";
        makeLevel();
        toString(*e.field, false);
        toString(*e.target, true);
        removeLevel();
    }

    void toString(const ConExpr& e) {
        stream << "ConExpr ";
        write(stream, context.findName(e.type->con));
    }

    void toString(const TupExpr& e) {
        stream << "TupExpr";
        makeLevel();
        auto arg = e.args;
        while(arg) {
            toString(arg->item, !arg->next);
            arg = arg->next;
        }
        removeLevel();
    }

    void toString(const TupUpdateExpr& e) {
        stream << "TupUpdateExpr ";
    }

    void toString(const ArrayExpr& e) {
        stream << "ArrayExpr ";
        makeLevel();
        auto arg = e.args;
        while(arg) {
            toString(*arg->item, !arg->next);
            arg = arg->next;
        }
        removeLevel();
    }

    void toString(const MapExpr& e) {
        stream << "MapExpr ";
    }

    void toString(const FormatExpr& e) {
        stream << "FormatExpr ";
        makeLevel();
        auto chunk = e.format;
        while(chunk) {
            toString(chunk->item, !chunk->next);
            chunk = chunk->next;
        }
        removeLevel();
    }

    void toString(const CaseExpr& e) {
        stream << "CaseExpr ";
        makeLevel();
        auto a = e.alts;
        toString(*e.pivot, a == nullptr);

        while(a) {
            toString(a->item, a->next == nullptr);
            a = a->next;
        }
        removeLevel();
    }

    void toString(const RetExpr& e) {
        stream << "RetExpr ";
        if(e.value) {
            makeLevel();
            toString(*e.value, true);
            removeLevel();
        }
    }

    void toString(const FunExpr& e) {
        stream << "FunExpr (";
        if(e.args) {
            auto arg = e.args;
            while(arg) {
                write(stream, context.findName(arg->item.name));
                if(arg->next) stream << ", ";
                arg = arg->next;
            }
        }
        stream << ')';

        makeLevel();
        toString(*e.body, true);
        removeLevel();
    }

    void toString(const Alt& alt, bool last) {
        toStringIntro(last);
        stream << "Alt";
        makeLevel();
        toString(*alt.pat, false);
        toString(*alt.expr, true);
        removeLevel();
    }

    void toString(const FunDecl& e) {
        stream << "FunDecl ";
        write(stream, context.findName(e.name));

        if(e.implicitReturn) {
            stream << " <implicit return> ";
        }

        makeLevel();
        toString(e.constraints, !e.args && !e.ret && !e.body);

        if(e.args) {
            auto arg = e.args;
            while(arg) {
                toStringIntro(arg->next == nullptr && e.ret == nullptr && e.body == nullptr);

                stream << "Arg ";
                write(stream, context.findName(arg->item.name));

                makeLevel();
                if(arg->item.type) toString(*arg->item.type, arg->item.def == nullptr);
                if(arg->item.def) toString(*arg->item.def, true);
                removeLevel();

                arg = arg->next;
            }
        }

        if(e.ret) {
            toStringIntro(e.body == nullptr);
            stream << "Result";
            makeLevel();
            toString(*e.ret, true);
            removeLevel();
        }

        if(e.body) {
            toStringIntro(true);
            stream << "Body";
            makeLevel();
            toString(*e.body, true);
            removeLevel();
        }
        removeLevel();
    }

    void toString(const AliasDecl& e) {
        stream << "AliasDecl ";
        toString(*e.type);
        makeLevel();
        toString(*e.target, true);
        removeLevel();
    }

    void toString(const DataDecl& e) {
        stream << "DataDecl ";
        toString(*e.type);
        makeLevel();
        toString(e.constraints, false);

        auto con = e.cons;
        while(con) {
            toString(con->item, con->next == nullptr);
            con = con->next;
        }
        removeLevel();
    }

    void toString(const ForeignDecl& e) {
        stream << "ForeignDecl ";
        auto name = context.find(e.externName);
        stream.write(name.text, name.textLength);
        stream << ' ';

        auto localName = context.find(e.localName);
        if(localName.textLength > 0) {
            stream.write(localName.text, localName.textLength);
        } else {
            stream.write(name.text, name.textLength);
        }

        makeLevel();
        toString(*e.type, true);
        removeLevel();
    }

    void toString(const StmtDecl& e) {
        stream << "StmtDecl";
        makeLevel();
        toString(*e.expr, true);
        removeLevel();
    }

    void toString(const ClassDecl& e) {
        stream << "ClassDecl ";
        toString(*e.type);

        makeLevel();
        toString(e.constraints, e.decls == nullptr);

        auto d = e.decls;
        while(d) {
            toString(*d->item, d->next == nullptr);
            d = d->next;
        }
        removeLevel();
    }

    void toString(const InstanceDecl& e) {
        stream << "InstanceDecl ";
        makeLevel();
        toString(*e.type, e.decls == nullptr);

        auto d = e.decls;
        while(d) {
            toString(*d->item, d->next == nullptr);
            d = d->next;
        }
        removeLevel();
    }

    void toString(const AttrDecl& e) {
        stream << "AttrDecl ";
        write(stream, context.findName(e.name));

        if(e.type) {
            makeLevel();
            toString(*e.type, true);
            removeLevel();
        }
    }

    void toString(const FormatChunk& f, bool last) {
        auto name = context.find(f.string);
        if(f.format) {
            toString(*f.format, name.textLength > 0 ? false : last);
        }

        if(name.textLength > 0) {
            toStringIntro(last);
            stream << "LitExpr \"";
            stream.write(name.text, name.textLength);
            stream << '"';
        }
    }

    void toString(const IfCase& c, bool last) {
        toStringIntro(last);
        stream << "IfCase ";
        makeLevel();
        toString(*c.cond, false);
        toString(*c.then, true);
        removeLevel();
    }

    void toString(const TupArg& arg, bool last) {
        toStringIntro(last);
        stream << "Field ";
        auto name = context.find(arg.name);
        if(name.textLength > 0) {
            stream.write(name.text, name.textLength);
        } else {
            stream << "<anonymous>";
        }

        makeLevel();
        toString(*arg.value, true);
        removeLevel();
    }

    void toString(const SimpleType& t) {
        auto name = context.find(t.name);
        if(name.textLength > 0) {
            stream.write(name.text, name.textLength);
            stream << ' ';
        }

        if(t.kind) {
            stream << '(';
            auto k = t.kind;
            while(k) {
                write(stream, context.findName(k->item));
                if(k->next) stream << ", ";
                k = k->next;
            }
            stream << ')';
        }
    }

    void toString(const Con& c, bool last) {
        auto name = context.find(c.name);
        if(name.textLength > 0) {
            toStringIntro(last);
            stream << "Constructor ";
            stream.write(name.text, name.textLength);

            makeLevel();
            if(c.attributes) {
                toStringIntro(c.content == nullptr);
                stream << "<attributes>";
                toString(c.attributes);
            }

            if(c.content) {
                toString(*c.content, true);
            }

            removeLevel();
        }
    }

    void toString(const Attribute& attribute, bool last) {
        toStringIntro(last);
        stream << "Attribute ";
        auto name = context.find(attribute.name);
        if(name.textLength > 0) {
            stream.write(name.text, name.textLength);
        }

        toString(attribute.args);
    }

    template<class T>
    void toString(List<T>* list) {
        if(list) {
            makeLevel();
            while(list) {
                toString(list->item, list->next == nullptr);
                list = list->next;
            }
            removeLevel();
        }
    }

    void toStringIntro(bool last) {
        stream << '\n';
        makeIndent(last);
        stream.write(indentStack, indentStart);
    }

    void toString(const Expr& expr, bool last) {
        toStringIntro(last);
        toString(expr);
    }

    void toString(const Decl& decl, bool last) {
        toStringIntro(last);
        toString(decl);
    }

    void toString(const Fixity& fixity, bool last) {
        toStringIntro(last);
        toString(fixity);
    }

    void toString(const Type& type, bool last) {
        toStringIntro(last);
        toString(type);
    }

    void toString(const Pat& pat, bool last) {
        toStringIntro(last);
        toString(pat);
    }

    void toString(const TupField& field, bool last) {
        toStringIntro(last);
        toString(field);
    }

    void toString(const ArgDecl& arg, bool last) {
        toStringIntro(last);
        toString(arg);
    }

    void toString(const VarDecl& decl, bool last) {
        toStringIntro(last);
        toString(decl);
    }

    void toString(const Import& import, bool last) {
        toStringIntro(last);
        toString(import);
    }

    void toString(const Constraint& constraint, bool last) {
        toStringIntro(last);
        toString(constraint);
    }

    void toString(const Literal& literal) {
        switch(literal.type) {
            case Literal::Int:
                stream << literal.i;
                break;
            case Literal::Float:
                stream << literal.f;
                break;
            case Literal::Char:
                stream << literal.c;
                break;
            case Literal::String: {
                stream << '"';
                write(stream, context.findName(literal.s));
                stream << '"';
                break;
            }
            case Literal::Bool:
                if(literal.i > 0) stream << "True";
                else stream << "False";
                break;
        }
    }

    void toString(const Type& type) {
        toString(type.attributes, "Type", [&] {
            switch(type.kind) {
                case Type::Error:
                    stream << "<parse error>";
                    break;
                case Type::Unit:
                    stream << "UnitType";
                    break;
                case Type::Con: {
                    auto name = context.find(((const ConType&)type).con);
                    stream << "ConType ";
                    stream.write(name.text, name.textLength);
                    break;
                }
                case Type::Ptr:
                    stream << "PtrType ";
                    makeLevel();
                    toString(*((const PtrType&)type).type, true);
                    removeLevel();
                    break;
                case Type::Ref:
                    stream << "RefType ";
                    makeLevel();
                    toString(*((const RefType&)type).type, true);
                    removeLevel();
                    break;
                case Type::Val:
                    stream << "ValType ";
                    makeLevel();
                    toString(*((const ValType&)type).type, true);
                    removeLevel();
                    break;
                case Type::Gen: {
                    stream << "GenType ";
                    auto name = context.find(((const GenType&)type).con);
                    stream.write(name.text, name.textLength);
                    break;
                }
                case Type::Tup:
                    toString((const TupType&)type);
                    break;
                case Type::Fun:
                    toString((const FunType&)type);
                    break;
                case Type::App:
                    toString((const AppType&)type);
                    break;
                case Type::Arr:
                    stream << "ArrType ";
                    makeLevel();
                    toString(*((const ArrType&)type).type, true);
                    removeLevel();
                    break;
                case Type::Map:
                    stream << "MapType ";
                    makeLevel();
                    toString(*((const MapType&)type).from, false);
                    toString(*((const MapType&)type).to, true);
                    removeLevel();
                    break;
            }
        });
    }

    void toString(const AppType& type) {
        stream << "AppType ";
        makeLevel();
        toString(*type.base, false);
        auto a = type.apps;
        while(a) {
            toString(*a->item, a->next == nullptr);
            a = a->next;
        }
        removeLevel();
    }

    void toString(const TupField& field) {
        stream << "Field ";
        if(field.name) {
            write(stream, context.findName(field.name));
        } else {
            stream << "<anonymous>";
        }

        makeLevel();
        toString(*field.type, true);
        removeLevel();
    }

    void toString(const TupType& type) {
        stream << "TupType ";
        makeLevel();
        auto a = type.fields;
        while(a) {
            toString(a->item, a->next == nullptr);
            a = a->next;
        }
        removeLevel();
    }

    void toString(const ArgDecl& arg) {
        stream << "Arg ";
        auto name = context.find(arg.name);
        if(name.textLength > 0) {
            stream.write(name.text, name.textLength);
        } else {
            stream << "<anonymous>";
        }

        makeLevel();
        toString(*arg.type, true);
        removeLevel();
    }

    void toString(const FunType& type) {
        stream << "FunType ";
        makeLevel();

        auto arg = type.args;
        while(arg) {
            toString(arg->item, false);
            arg = arg->next;
        }

        if(type.ret) {
            toString(*type.ret, true);
        } else {
            toStringIntro(true);
            stream << "UnitType";
        }

        removeLevel();
    }

    void toString(const Pat& pat) {
        switch(pat.kind) {
            case Pat::Error:
                stream << "<parse error>";
                break;
            case Pat::Var: {
                stream << "VarPat ";
                auto name = context.find(((const VarPat&)pat).var);
                stream.write(name.text, name.textLength);
                break;
            }
            case Pat::Lit:
                stream << "LitPat ";
                toString(((const LitPat&)pat).lit);
                break;
            case Pat::Any:
                stream << "AnyPat";
                break;
            case Pat::Tup: {
                stream << "TupPat";
                auto fields = ((const TupPat&)pat).fields;
                makeLevel();
                while(fields) {
                    toStringIntro(fields->next == nullptr);
                    stream << "Field ";
                    auto name = context.find(fields->item.field);
                    if(name.textLength > 0) {
                        stream.write(name.text, name.textLength);
                    }

                    makeLevel();
                    toString(*fields->item.pat, true);
                    removeLevel();

                    fields = fields->next;
                }
                removeLevel();
                break;
            }
            case Pat::Con: {
                stream << "ConPat ";
                auto& con = ((const ConPat&)pat);
                write(stream, context.findName(con.constructor));

                if(con.pats) {
                    makeLevel();
                    toString(*con.pats, true);
                    removeLevel();
                }
                break;
            }
            case Pat::Array: {
                stream << "ArrayPat ";
                auto& array = ((const ArrayPat&)pat);
                auto p = array.pats;

                makeLevel();
                while(p) {
                    toString(*p->item, p->next == nullptr);
                    p = p->next;
                }
                removeLevel();
                break;
            }
            case Pat::Rest: {
                stream << "RestPat ";
                auto& rest = ((const RestPat&)pat);
                write(stream, context.findName(rest.var));
                break;
            }
            case Pat::Range: {
                stream << "RangePat ";
                auto& range = ((const RangePat&)pat);

                makeLevel();
                toString(*range.from, false);
                toString(*range.to, true);
                removeLevel();
                break;
            }
        }
    }

    void toString(const Constraint& constraint) {
        switch(constraint.kind) {
            case Constraint::Error:
                stream << "<parse error>";
                break;
            case Constraint::Any: {
                stream << "AnyConstraint ";
                auto name = context.find(((const AnyConstraint&)constraint).name);
                stream.write(name.text, name.textLength);
                break;
            }
            case Constraint::Class: {
                auto& c = (const ClassConstraint&)constraint;
                stream << "ClassConstraint ";
                toString(*c.type);
                break;
            }
            case Constraint::Field: {
                auto& c = (const FieldConstraint&)constraint;
                stream << "FieldConstraint ";

                write(stream, context.findName(c.typeName));
                stream << '.';
                write(stream, context.findName(c.fieldName));

                makeLevel();
                toString(*c.type, true);
                removeLevel();
                break;
            }
            case Constraint::Function: {
                auto& c = (const FunctionConstraint&)constraint;
                stream << "FunctionConstraint ";
                write(stream, context.findName(c.name));

                makeLevel();
                toString(c.type, true);
                removeLevel();
                break;
            }
        }
    }

    void toString(List<Constraint*>* constraints, bool last) {
        if(constraints) {
            toStringIntro(last);
            stream << "<constraints>";
            makeLevel();
            while(constraints) {
                toString(*constraints->item, constraints->next == nullptr);
                constraints = constraints->next;
            }
            removeLevel();
        }
    }

    template<class F>
    void toString(List<Attribute>* attributes, const char* name, F&& f) {
        if(attributes) {
            stream << name;
            makeLevel();
            toStringIntro(false);

            stream << "<attributes>";
            makeLevel();
            while(attributes) {
                toString(attributes->item, attributes->next == nullptr);
                attributes = attributes->next;
            }
            removeLevel();

            toStringIntro(true);
            f();
            removeLevel();
        } else {
            f();
        }
    }

    char indentStack[1024];
    U32 indentStart = 0;

    Context& context;
    std::ostream& stream;
};

void printModule(std::ostream& stream, Context& context, const Module& module) {
    Printer{context, stream}.toString(module);
}

void printDecl(std::ostream& stream, Context& context, const Decl& decl) {
    Printer{context, stream}.toString(decl);
}

void printExpr(std::ostream& stream, Context& context, const Expr& expr) {
    Printer{context, stream}.toString(expr);
}