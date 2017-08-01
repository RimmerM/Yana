#include "ast_print.h"

using namespace ast;

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
        switch(decl.kind) {
            case Decl::Error: stream << "<parse error>"; break;
            case Decl::Fun: toString((const FunDecl&)decl); break;
            case Decl::Alias: toString((const AliasDecl&)decl); break;
            case Decl::Data: toString((const DataDecl&)decl); break;
            case Decl::Foreign: toString((const ForeignDecl&)decl); break;
            case Decl::Stmt: toString((const StmtDecl&)decl); break;
        }
    }

    void toString(const Module& mod) {
        stream << "Module ";
        Size max = mod.decls.size();
        if(max) {
            makeLevel();
            for(Size i = 0; i < max - 1; i++) {
                toString(*mod.decls[i], false);
            }
            toString(*mod.decls[max-1], true);
            removeLevel();
        }
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

        char buffer[32];
        switch(e.literal.type) {
            case Literal::Int:
                stream << e.literal.i;
                break;
            case Literal::Float:
                stream << e.literal.f;
                break;
            case Literal::Char:
                stream << e.literal.c;
                break;
            case Literal::String: {
                stream << '"';
                auto name = context.find(e.literal.s);
                stream.write(name.name, name.length);
                stream << '"';
                break;
            }
            case Literal::Bool:
                if(e.literal.i) stream << "True";
                else stream << "False";
                break;
        }
    }

    void toString(const VarExpr& e) {
        stream << "VarExpr ";
        auto name = context.find(e.name);
        stream.write(name.name, name.length);
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
        auto name = context.find(e.op->name);
        stream.write(name.name, name.length);
        makeLevel();
        toString(*e.lhs,  false);
        toString(*e.rhs, true);
        removeLevel();
    }

    void toString(const PrefixExpr& e) {
        stream << "PrefixExpr ";
        auto name = context.find(e.op->name);
        stream.write(name.name, name.length);
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

    void toString(const DeclExpr& e) {
        stream << "DeclExpr ";
        auto name = context.find(e.name);
        stream.write(name.name, name.length);
        if(e.isRef) stream << " <ref> ";
        if(e.content) {
            makeLevel();
            toString(*e.content, true);
            removeLevel();
        } else {
            stream << " <empty> ";
        }
    }

    void toString(const WhileExpr& e) {
        stream << "WhileExpr";
        makeLevel();
        toString(*e.cond, false);
        toString(*e.loop, true);
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
        stream << '(';
        toString(*e.kind);
        stream << ')';
        makeLevel();
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
        auto name = context.find(e.type->con);
        stream.write(name.name, name.length);
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
        while(a) {
            toString(a->item, a->next == nullptr);
            a = a->next;
        }
        removeLevel();
    }

    void toString(const RetExpr& e) {
        stream << "RetExpr ";
        makeLevel();
        toString(*e.value, true);
        removeLevel();
    }

    void toString(const FunExpr& e) {
        stream << "FunExpr (";
        if(e.args) {
            auto arg = e.args;
            while(arg) {
                auto name = context.find(arg->item.name);
                stream.write(name.name, name.length);
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
        stream << "alt: ";
        toString(*alt.expr);
    }

    void toString(const FunDecl& e) {
        stream << "FunDecl ";
        auto name = context.find(e.name);
        stream.write(name.name, name.length);
        stream << '(';
        if(e.args) {
            auto arg = e.args;
            while(arg) {
                auto argName = context.find(arg->item.name);
                stream.write(argName.name, argName.length);
                if(arg->next) stream << ", ";
                arg = arg->next;
            }
        }
        stream << ')';

        if(e.body) {
            makeLevel();
            toString(*e.body, true);
            removeLevel();
        }
    }

    void toString(const AliasDecl& e) {
        stream << "AliasDecl ";
        auto name = context.find(e.type->name);
        stream.write(name.name, name.length);
        stream << " = ";
        toString(*e.target);
    }

    void toString(const DataDecl& e) {
        stream << "DataDecl ";
        toString(*e.type);
        makeLevel();
        auto con = e.cons;
        while(con) {
            toString(con->item, con->next == nullptr);
            con = con->next;
        }
        removeLevel();
    }

    void toString(const ForeignDecl& e) {
        stream << "ForeignDecl ";
        auto name = context.find(e.localName);
        stream.write(name.name, name.length);
        stream << " : ";
        toString(*e.type);
    }

    void toString(const StmtDecl& e) {
        stream << "StmtDecl";
        makeLevel();
        toString(*e.expr, true);
        removeLevel();
    }

    void toString(const FormatChunk& f, bool last) {
        auto name = context.find(f.string);
        if(f.format) {
            toString(*f.format, name.length ? false : last);
        }

        if(name.length) {
            toStringIntro(last);
            stream << "LitExpr \"";
            stream.write(name.name, name.length);
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
        if(name.length) {
            stream.write(name.name, name.length);
        } else {
            stream << "<unnamed>";
        }

        makeLevel();
        toString(*arg.value, true);
        removeLevel();
    }

    void toString(const SimpleType& t) {
        auto name = context.find(t.name);
        if(name.length) {
            stream.write(name.name, name.length);
            stream << ' ';
        }
    }

    void toString(const Con& c, bool last) {
        auto name = context.find(c.name);
        if(name.length) {
            toStringIntro(last);
            stream << "Constructor ";
            stream.write(name.name, name.length);
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

    void toString(const Type& type) {
        switch(type.kind) {
            case Type::Error:
                stream << "<parse error>";
                break;
            case Type::Unit:
                stream << "()";
                break;
            case Type::Con: {
                auto name = context.find(((const ConType&)type).con);
                stream.write(name.name, name.length);
                break;
            }
            case Type::Ptr:
                stream << '#';
                toString(*((const PtrType&)type).type);
                break;
            case Type::Gen: {
                stream << "gen";
                auto name = context.find(((const GenType&)type).con);
                stream.write(name.name, name.length);
                break;
            }
            case Type::Tup:
                stream << "tuple";
                break;
            case Type::Fun:
                stream << "fun";
                break;
            case Type::App:
                stream << "app ";
                toString(*((const AppType&)type).base);
                break;
            case Type::Arr:
                stream << "array(";
                toString(*((const ArrType&)type).type);
                stream << ")";
                break;
            case Type::Map:
                stream << "map(";
                toString(*((const MapType&)type).from);
                stream << " -> ";
                toString(*((const MapType&)type).to);
                stream << ")";
                break;
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