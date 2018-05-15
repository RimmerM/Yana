#include <fstream>
#include "ast.h"
#include "../../compiler/context.h"

using namespace js;

struct StringSeg {
    const char* string;
    U32 length;
};

struct CodeBuilder {
    std::ostream& writer;
    Context& context;

    bool minify;
    U32 indentation = 0;

    StringSeg space;
    StringSeg comma;
    StringSeg parenBlockStart;
    StringSeg doBlockStart;
    StringSeg doCondStart;
    StringSeg elseHead;
    StringSeg indent;
    StringSeg colon;

    CodeBuilder(std::ostream& writer, Context& context, StringSeg indent, bool minify): writer(writer), context(context), minify(minify), indent(indent) {
        if(minify) {
            space.string = "";
            space.length = 0;

            comma.string = ",";
            comma.length = 1;

            parenBlockStart.string = "){";
            parenBlockStart.length = 2;

            doBlockStart.string = "do{";
            doBlockStart.length = 3;

            doCondStart.string = "}while(";
            doCondStart.length = 7;

            elseHead.string = "}else{";
            elseHead.length = 6;

            colon.string = ":";
            colon.length = 1;
        } else {
            space.string = " ";
            space.length = 1;

            comma.string = ", ";
            comma.length = 2;

            parenBlockStart.string = ") {";
            parenBlockStart.length = 3;

            doBlockStart.string = "do {";
            doBlockStart.length = 4;

            doCondStart.string = "} while(";
            doCondStart.length = 8;

            elseHead.string = "} else {";
            elseHead.length = 8;

            colon.string = ": ";
            colon.length = 2;
        }
    }

    void startLine() {
        if(!minify) {
            for(U32 i = 0; i < indentation; i++) {
                append(indent);
            }
        }
    }

    void endLine() {
        if(!minify) {
            writer << '\n';
        }
    }

    template<class F> void withLevel(F&& f) {
        indentation++;
        f();
        indentation--;
    }

    template<class F> void sepBy(U32 count, StringSeg sep, F&& f) {
        if(sep.length > 0 && sep.string[0] == '\n') {
            for(U32 i = 0; i < count; i++) {
                if(i == count - 1) {
                    f(i);
                } else {
                    f(i);
                    endLine();
                    startLine();
                    writer.write(sep.string + 1, sep.length - 1);
                }
            }
        } else if(sep.length > 0 && sep.string[sep.length - 1] == '\n') {
            for(U32 i = 0; i < count; i++) {
                if(i == count - 1) {
                    f(i);
                } else {
                    f(i);
                    writer.write(sep.string, sep.length - 1);
                    endLine();
                    startLine();
                }
            }
        } else {
            for(U32 i = 0; i < count; i++) {
                if(i == count - 1) {
                    f(i);
                } else {
                    f(i);
                    append(sep);
                }
            }
        }
    }

    void append(StringSeg seg) {
        writer.write(seg.string, seg.length);
    }

    void line(StringSeg seg) {
        startLine();
        append(seg);
        endLine();
    }

    void line(char c) {
        startLine();
        writer << c;
        endLine();
    }
};

static void appendExpr(CodeBuilder& b, Expr* e);
static void formatFun(CodeBuilder& b, Id name, U32 localId, bool named, Variable* args, U32 argCount, Stmt* body);

static bool isFieldName(const char* string, U32 length) {
    for(U32 i = 0; i < length; i++) {
        auto c = string[i];
        if(c >= 'a' && c <= 'z') continue;
        if(c >= 'A' && c <= 'Z') continue;
        if(c >= '0' && c <= '9') continue;
        if(c == '_') continue;

        return false;
    }

    return true;
}

static void appendVarName(CodeBuilder& b, Id name, U32 localId) {
    if(!b.minify && name) {
        auto id = b.context.find(name);
        b.writer.write(id.text, id.textLength);
        b.writer << '_';
    }

    auto index = localId % 52;
    if(index < 26) {
        b.writer << char('a' + index);
    } else {
        b.writer << char('A' + (index - 26));
    }

    auto remaining = localId % 52;
    while(remaining > 62) {
        index = remaining % 62;
        remaining /= 62;

        if(index < 26) {
            b.writer << char('a' + index);
        } else if(index < 52) {
            b.writer << char('A' + (index - 26));
        } else {
            b.writer << char('0' + (index - 52));
        }
    }
}

static void appendString(CodeBuilder& b, Id string) {
    b.writer << '"';
    auto id = b.context.find(string);
    for(U32 i = 0; i < id.textLength; i++) {
        auto c = id.text[i];
        if(c == '\b') {
            b.writer << "\\b";
        } else if(c == '\n') {
            b.writer << "\\n";
        } else if(c == '\\') {
            b.writer << "\\\\";
        } else if(c == '"') {
            b.writer << "\\\"";
        } else {
            b.writer << c;
        }
    }
    b.writer << '"';
}

static void appendObjectKey(CodeBuilder& b, Expr* key, Expr* value) {
    if(key->type == Expr::String) {
        auto s = ((StringExpr*)key)->string;
        auto id = b.context.find(s);

        if(isFieldName(id.text, id.textLength)) {
            b.writer.write(id.text, id.textLength);
        } else {
            b.writer << '"';
            b.writer.write(id.text, id.textLength);
            b.writer << '"';
        }
    } else {
        b.writer << '[';
        appendExpr(b, key);
        b.writer << ']';
    }

    b.append(b.colon);
    appendExpr(b, value);
}

static void appendExpr(CodeBuilder& b, Expr* e) {
    switch(e->type) {
        case Expr::String:
            appendString(b, ((StringExpr*)e)->string);
            break;
        case Expr::Float:
            b.writer << ((FloatExpr*)e)->f;
            break;
        case Expr::Int:
            b.writer << ((IntExpr*)e)->i;
            break;
        case Expr::Bool:
            b.writer << (((BoolExpr*)e)->b ? "true" : "false");
            break;
        case Expr::Null:
            b.writer << "null";
            break;
        case Expr::Undefined:
            b.writer << "undefined";
            break;
        case Expr::Array: {
            auto a = (ArrayExpr*)e;

            b.writer << '[';
            for(U32 i = 0; i < a->count; i++) {
                if(i == a->count - 1) {
                    appendExpr(b, a->content[i]);
                } else {
                    appendExpr(b, a->content[i]);
                    b.append(b.comma);
                }
            }
            b.writer << ']';
            break;
        }
        case Expr::Object: {
            auto o = (ObjectExpr*)e;

            b.writer << '{';
            for(U32 i = 0; i < o->count; i++) {
                if(i == o->count - 1) {
                    appendObjectKey(b, o->keys[i], o->values[i]);
                } else {
                    appendObjectKey(b, o->keys[i], o->values[i]);
                    b.append(b.comma);
                }
            }
            b.writer << '}';
            break;
        }
        case Expr::Var: {
            auto var = (VarExpr*)e;
            appendVarName(b, var->var->name, var->var->localId);
            break;
        }
        case Expr::Field: {
            auto f = (FieldExpr*)e;
            appendExpr(b, f->arg);

            bool isField = false;
            if(f->field->type == Expr::String) {
                auto field = b.context.find(((StringExpr*)f->field)->string);
                if(isFieldName(field.text, field.textLength)) {
                    b.writer << '.';
                    b.writer.write(field.text, field.textLength);
                    isField = true;
                }
            }

            if(!isField) {
                b.writer << '[';
                appendExpr(b, f->field);
                b.writer << ']';
            }

            break;
        }
        case Expr::Prefix: {
            auto p = (PrefixExpr*)e;

            b.writer << '(';
            auto op = b.context.find(p->op);
            b.writer.write(op.text, op.textLength);
            appendExpr(b, p->arg);
            b.writer << ')';
            break;
        }
        case Expr::Infix: {
            auto i = (InfixExpr*)e;

            b.writer << '(';
            appendExpr(b, i->lhs);
            b.append(b.space);
            auto op = b.context.find(i->op);
            b.writer.write(op.text, op.textLength);
            b.append(b.space);
            appendExpr(b, i->rhs);
            b.writer << ')';
            break;
        }
        case Expr::If: {
            auto i = (IfExpr*)e;

            b.writer << '(';
            appendExpr(b, i->cond);
            b.append(b.space);
            b.writer << '?';
            b.append(b.space);
            appendExpr(b, i->then);
            b.append(b.space);
            b.writer << ':';
            b.append(b.space);
            appendExpr(b, i->otherwise);
            b.writer << ')';
            break;
        }
        case Expr::Assign: {
            auto a = (AssignExpr*)e;

            appendExpr(b, a->target);
            b.append(b.space);
            b.writer << '=';
            b.append(b.space);
            appendExpr(b, a->value);
            break;
        }
        case Expr::Call: {
            auto c = (CallExpr*)e;

            appendExpr(b, c->target);
            b.writer << '(';
            for(U32 i = 0; i < c->count; i++) {
                if(i == c->count - 1) {
                    appendExpr(b, c->args[i]);
                } else {
                    appendExpr(b, c->args[i]);
                    b.append(b.comma);
                }
            }
            b.writer << ')';
            break;
        }
        case Expr::Fun: {
            auto f = (FunExpr*)e;
            formatFun(b, f->name, 0, false, f->args, f->argCount, f->body);
            break;
        }
    }
}

static void appendVarDecl(CodeBuilder& b, DeclStmt* v) {
    appendVarName(b, v->v->name, v->v->localId);
    if(v->value) {
        b.append(b.space);
        b.writer << '=';
        b.append(b.space);
        appendExpr(b, v->value);
    }
}

static void formatStmt(CodeBuilder& b, Stmt* stmt) {
    switch(stmt->type) {
        case Stmt::Block:
            b.line('{');
            b.withLevel([&] {
                auto block = (BlockStmt*)stmt;
                for(U32 i = 0; i < block->count; i++) {
                    formatStmt(b, block->stmts[i]);
                }
            });
            b.line('}');
            break;
        case Stmt::Exp:
            b.startLine();
            appendExpr(b, ((ExprStmt*)stmt)->expr);
            b.writer << ';';
            b.endLine();
            break;
        case Stmt::If: {
            auto c = (IfStmt*)stmt;

            b.startLine();
            b.writer << "if(";
            appendExpr(b, c->cond);
            b.append(b.parenBlockStart);
            b.withLevel([&] {
                formatStmt(b, c->then);
            });

            if(c->otherwise) {
                b.line(b.elseHead);
                b.withLevel([&] {
                    formatStmt(b, c->otherwise);
                });
                b.line('}');
            }

            b.line('}');
            break;
        }
        case Stmt::While: {
            auto c = (WhileStmt*)stmt;

            b.startLine();
            b.writer << "while(";
            appendExpr(b, c->cond);
            b.append(b.parenBlockStart);
            b.withLevel([&] {
                formatStmt(b, c->body);
            });

            b.line('}');
            break;
        }
        case Stmt::DoWhile: {
            auto c = (DoWhileStmt*)stmt;

            b.startLine();
            b.append(b.doBlockStart);
            b.withLevel([&] {
                formatStmt(b, c->body);
            });

            b.startLine();
            b.append(b.doCondStart);
            appendExpr(b, c->cond);
            b.append({");", 2});
            b.endLine();
            break;
        }
        case Stmt::Break:
            b.line({"break;", 6});
            break;
        case Stmt::Continue:
            b.line({"continue;", 9});
            break;
        case Stmt::Labelled: {
            auto c = (LabelledStmt*)stmt;
            auto id = b.context.find(c->name);

            b.startLine();
            b.writer.write(id.text, id.textLength);
            b.writer << ':';
            b.append(b.space);
            b.endLine();
            formatStmt(b, c->content);
            break;
        }
        case Stmt::Return: {
            auto c = (ReturnStmt*)stmt;
            b.startLine();
            if(c->value) {
                b.append({"return ", 7});
                appendExpr(b, c->value);
                b.writer << ';';
            } else {
                b.append({"return;", 7});
            }

            b.endLine();
            break;
        }
        case Stmt::Decl: {
            auto c = (DeclStmt*)stmt;
            b.startLine();
            b.append({"var ", 4});
            appendVarDecl(b, c);
            b.writer << ';';
            b.endLine();
            break;
        }
        case Stmt::Var: {
            auto c = (VarStmt*)stmt;
            b.startLine();
            b.append({"var ", 4});

            for(U32 i = 0; i < c->count; i++) {
                if(i == c->count - 1) {
                    appendVarDecl(b, &c->values[i]);
                    b.writer << ';';
                } else {
                    appendVarDecl(b, &c->values[i]);
                    b.append(b.comma);
                }
            }

            b.endLine();
            break;
        }
        case Stmt::Fun: {
            auto c = (FunStmt*)stmt;
            formatFun(b, c->name, c->localId, true, c->args, c->argCount, c->body);
            break;
        }
    }
}

static void formatFun(CodeBuilder& b, Id name, U32 localId, bool named, Variable* args, U32 argCount, Stmt* body) {
    if(named) {
        b.startLine();
        b.writer << "function ";
        appendVarName(b, name, localId);
        b.writer << '(';
    } else {
        b.writer << "function(";
    }

    if(argCount > 6 && !b.minify) {
        b.endLine();
        b.withLevel([&] {
            b.startLine();
            b.sepBy(argCount, {",\n", 2}, [&](U32 i) {
                appendVarName(b, args[i].name, args[i].localId);
            });
        });
        b.endLine();
        b.startLine();
    } else if(argCount > 0) {
        b.sepBy(argCount, b.comma, [&](U32 i) {
            appendVarName(b, args[i].name, args[i].localId);
        });
    }

    b.append(b.parenBlockStart);
    b.endLine();
    b.withLevel([&] {
        if(body->type == Stmt::Block) {
            auto block = (BlockStmt*)body;
            for(U32 i = 0; i < block->count; i++) {
                formatStmt(b, block->stmts[i]);
            }
        } else {
            formatStmt(b, body);
        }
    });
    b.line('}');
    b.endLine();
}

namespace js {

void formatFile(Context& context, std::ostream& to, File* file, bool minify) {
    CodeBuilder builder(to, context, {"  ", 2}, minify);
    for(auto stmt: file->statements) {
        formatStmt(builder, stmt);
    }
}

}
