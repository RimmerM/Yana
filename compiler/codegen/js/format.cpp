#include "gen.h"

/*
 * The JS AST as text.
 *
 * The only decision here that is not mechanical is parenthesization, and it is made from operator
 * precedence rather than by wrapping everything: `(a + b) | 0` is what the integer tower asks for
 * on almost every arithmetic instruction (Analysis-JS.md §2.1), and a formatter that parenthesized
 * unconditionally would turn that into an unreadable pile for the one case that matters most.
 */

namespace js {

namespace {

struct OpInfo {
    StringView text;
    U8 precedence;
};

// Precedence numbers from the grammar; low binds loosest. Only the levels this tree can build are
// listed - assignment and the conditional are handled where they are printed.
const OpInfo kBinaryOps[] = {
    { "*"_v, 12 }, { "/"_v, 12 }, { "%"_v, 12 },
    { "+"_v, 11 }, { "-"_v, 11 },
    { "<<"_v, 10 }, { ">>>"_v, 10 }, { ">>"_v, 10 },
    { "<"_v, 9 }, { "<="_v, 9 }, { ">"_v, 9 }, { ">="_v, 9 },
    { "==="_v, 8 }, { "!=="_v, 8 },
    { "=="_v, 8 }, { "!="_v, 8 },
    { "&"_v, 7 }, { "^"_v, 6 }, { "|"_v, 5 },
    { "&&"_v, 4 }, { "||"_v, 3 },
};

// Below everything, so a function expression is bare where it stands alone - a `return`, an
// initializer, an argument - and parenthesized everywhere else. `(function(){}).x` needs them and
// `return function(){}` does not, and one number decides both.
constexpr U8 kFunctionPrecedence = 1;
constexpr U8 kAssignPrecedence = 2;
constexpr U8 kTernaryPrecedence = 3;
constexpr U8 kUnaryPrecedence = 14;
constexpr U8 kCallPrecedence = 17;
constexpr U8 kAtomPrecedence = 18;

struct Format {
    Net::Writer& writer;
    Context& context;
    JsBase base;
    bool minify;
    U32 indentation = 0;

    void write(StringView text) { writer.writeString(text); }
    void write(char c) { writer.writeByte(c); }

    void name(Name value) {
        auto id = context.find(value.text);
        writer.writeString(StringView { id.text, id.textLength });
    }

    void space() { if(!minify) write(' '); }
    void newline() { if(!minify) write('\n'); }

    // Set by a label, which has already opened the line the statement it introduces continues.
    bool sameLine = false;

    void startLine() {
        if(sameLine) {
            sameLine = false;
            return;
        }

        if(minify) return;
        for(U32 i = 0; i < indentation; i++) write("  "_v);
    }

    template<class F>
    void withLevel(F&& f) {
        indentation++;
        f();
        indentation--;
    }
};

void writeInt(Format& f, I64 value) {
    f.writer.writeBytes(32, [&](Byte* buffer) { return show(value, (char*)buffer, 32); });
}

void writeNumber(Format& f, F64 value, bool integral) {
    // An integral value is printed through the integer path so that a number the IR holds as bits
    // comes out as an index rather than as `4.09600e+06`. Anything past the exactly-representable
    // range falls back to the general form, where it is a double in fact as well as in shape.
    if(integral && value >= -9007199254740992.0 && value <= 9007199254740992.0) {
        writeInt(f, I64(value));
        return;
    }

    f.writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
}

void writeStringLiteral(Format& f, StringId value) {
    auto id = f.context.find(value);
    f.write('"');

    for(U32 i = 0; i < id.textLength; i++) {
        auto c = id.text[i];
        switch(c) {
            case '"': f.write("\\\""_v); break;
            case '\\': f.write("\\\\"_v); break;
            case '\n': f.write("\\n"_v); break;
            case '\r': f.write("\\r"_v); break;
            case '\t': f.write("\\t"_v); break;
            default: f.write(c); break;
        }
    }

    f.write('"');
}

/*
 * Whether this interned text can be written after a dot.
 *
 * Every property name the generator builds goes through propertyName or literalName and is an
 * identifier by construction, but a `StringExpr` is also how a program's own string constant is
 * emitted, and one of those can appear as a key. So the text is checked rather than assumed - a
 * digit-leading or punctuated key stays in brackets, where it is correct.
 *
 * Reserved words are deliberately not excluded: `o.default` and `o.new` are legal property accesses,
 * and only a *binding* may not use one.
 */
bool identifierText(Format& f, StringId value) {
    auto id = f.context.find(value);
    if(!id.textLength) return false;

    for(U32 i = 0; i < id.textLength; i++) {
        auto c = id.text[i];
        auto ordinary = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';

        if(!ordinary && !(i && c >= '0' && c <= '9')) return false;
    }

    return true;
}

U8 precedenceOf(Format& f, JsPtr<Expr> pointer) {
    switch(f.base[pointer]->kind) {
        case Expr::Function: return kFunctionPrecedence;
        case Expr::Unary: return kUnaryPrecedence;
        case Expr::Binary: return kBinaryOps[U32(((BinaryExpr*)f.base[pointer])->op)].precedence;
        case Expr::Ternary: return kTernaryPrecedence;
        case Expr::Assign: return kAssignPrecedence;
        case Expr::Call:
        case Expr::Field:
        case Expr::Index: return kCallPrecedence;
        default: return kAtomPrecedence;
    }
}

void writeExpr(Format& f, JsPtr<Expr> pointer);
void writeBody(Format& f, StmtList body);

// The parameter list both a declaration and a function expression have.
void writeArgs(Format& f, JsList<Name, false>& args) {
    auto first = true;

    f.write('(');
    for(auto arg: args.contents(f.base)) {
        if(!first) {
            f.write(',');
            f.space();
        }

        first = false;
        f.name(arg);
    }

    f.write(')');
}

/*
 * Whether printing this expression starts with a `-`, which is a lexical question rather than a
 * structural one - see the `Neg` case in writeExpr.
 *
 * A negative number literal and a negation are the two, and nothing else can begin with one: an
 * operand that would be parenthesized starts with `(`, and every other leaf starts with a letter, a
 * digit or a quote. A `bigint` is included because `-1n` is a literal there exactly as it is here.
 */
bool startsWithMinus(Format& f, JsPtr<Expr> pointer) {
    auto expr = f.base[pointer];

    switch(expr->kind) {
        case Expr::Number: return ((NumberExpr*)expr)->value < 0;
        case Expr::BigInt: {
            auto value = (BigIntExpr*)expr;
            return value->isSigned && I64(value->value) < 0;
        }
        case Expr::Unary: return ((UnaryExpr*)expr)->op == UnaryOp::Neg;
        default: return false;
    }
}

// `expr` where it binds at least as tightly as the position it goes into, and `(expr)` otherwise.
void writeNested(Format& f, JsPtr<Expr> pointer, U8 required) {
    if(precedenceOf(f, pointer) < required) {
        f.write('(');
        writeExpr(f, pointer);
        f.write(')');
    } else {
        writeExpr(f, pointer);
    }
}

void writeExpr(Format& f, JsPtr<Expr> pointer) {
    auto expr = f.base[pointer];

    switch(expr->kind) {
        case Expr::Number: {
            auto number = (NumberExpr*)expr;
            writeNumber(f, number->value, number->integral);
            break;
        }
        case Expr::BigInt: {
            auto value = (BigIntExpr*)expr;
            if(value->isSigned) {
                writeInt(f, I64(value->value));
            } else {
                f.writer.writeBytes(32, [&](Byte* buffer) { return show(value->value, (char*)buffer, 32); });
            }

            f.write('n');
            break;
        }
        case Expr::String:
            writeStringLiteral(f, ((StringExpr*)expr)->value);
            break;
        case Expr::Bool:
            f.write(((BoolExpr*)expr)->value ? "true"_v : "false"_v);
            break;
        case Expr::Null:
            f.write("null"_v);
            break;
        case Expr::Undefined:
            f.write("undefined"_v);
            break;
        case Expr::Var:
            f.name(((VarExpr*)expr)->name);
            break;
        case Expr::Field: {
            auto field = (FieldExpr*)expr;
            writeNested(f, field->object, kCallPrecedence);
            f.write('.');
            f.name(field->field);
            break;
        }
        case Expr::Index: {
            auto index = (IndexExpr*)expr;
            writeNested(f, index->array, kCallPrecedence);

            /*
             * A constant key that is a valid identifier prints as `o.k` rather than `o["k"]`.
             *
             * The two are the same operation, and the emitter cannot always tell them apart: a place
             * into a narrow reference is `owner[key]` because the key is a *value* there, and that
             * key turns out to be a literal wherever the reference did not come through a parameter.
             * Deciding it here rather than at each construction site is what catches all of them.
             */
            auto key = f.base[index->index];
            if(key->kind == Expr::String && identifierText(f, ((StringExpr*)key)->value)) {
                f.write('.');
                f.name(Name { ((StringExpr*)key)->value });
                break;
            }

            f.write('[');
            writeExpr(f, index->index);
            f.write(']');
            break;
        }
        case Expr::Array: {
            auto array = (ArrayExpr*)expr;
            auto first = true;

            f.write('[');
            for(auto value: array->values.contents(f.base)) {
                if(!first) {
                    f.write(',');
                    f.space();
                }

                first = false;
                writeExpr(f, value);
            }
            f.write(']');
            break;
        }
        case Expr::Object: {
            auto object = (ObjectExpr*)expr;
            auto first = true;

            f.write('{');
            for(auto property: object->properties.contents(f.base)) {
                if(!first) f.write(',');
                f.space();
                first = false;

                f.name(property.key);
                f.write(':');
                f.space();
                writeExpr(f, property.value);
            }

            if(!first) f.space();
            f.write('}');
            break;
        }
        case Expr::Unary: {
            auto unary = (UnaryExpr*)expr;
            switch(unary->op) {
                case UnaryOp::Neg: f.write('-'); break;
                case UnaryOp::Not: f.write('!'); break;
                case UnaryOp::BitNot: f.write('~'); break;
            }

            /*
             * A space between two `-` signs, because `--` is one token in JavaScript.
             *
             * `-(-2147483648)` is what negating `Int`'s most negative value is - the literal has no
             * positive form, so the front end carries it as a negation of a negative constant - and
             * printed without this it comes out as `--2147483648`, which the host parses as a prefix
             * decrement and rejects at *parse* time. So the whole file fails to load rather than one
             * expression misbehaving, which is why nothing caught it until a program contained the
             * constant at all.
             *
             * A space rather than parentheses because it is the smaller output and the ambiguity is
             * purely lexical: the precedence is already right, and `- -2147483648` is one token pair
             * where `--2147483648` is another.
             */
            if(unary->op == UnaryOp::Neg && startsWithMinus(f, unary->value)) f.space();

            writeNested(f, unary->value, kUnaryPrecedence);
            break;
        }
        case Expr::Binary: {
            auto binary = (BinaryExpr*)expr;
            auto& op = kBinaryOps[U32(binary->op)];

            // Every operator here is left-associative, so the right operand needs one level more
            // than the left to stay unparenthesized.
            writeNested(f, binary->lhs, op.precedence);
            f.space();
            f.write(op.text);
            f.space();
            writeNested(f, binary->rhs, U8(op.precedence + 1));
            break;
        }
        case Expr::Ternary: {
            auto ternary = (TernaryExpr*)expr;
            writeNested(f, ternary->cond, U8(kTernaryPrecedence + 1));
            f.space();
            f.write('?');
            f.space();
            writeNested(f, ternary->then, kAssignPrecedence);
            f.space();
            f.write(':');
            f.space();
            writeNested(f, ternary->otherwise, kAssignPrecedence);
            break;
        }
        case Expr::Assign: {
            auto assign = (AssignExpr*)expr;
            writeNested(f, assign->target, kCallPrecedence);
            f.space();
            f.write('=');
            f.space();
            writeNested(f, assign->value, kAssignPrecedence);
            break;
        }
        case Expr::Call: {
            auto call = (CallExpr*)expr;
            auto first = true;

            writeNested(f, call->callee, kCallPrecedence);
            f.write('(');
            for(auto arg: call->args.contents(f.base)) {
                if(!first) {
                    f.write(',');
                    f.space();
                }

                first = false;
                writeExpr(f, arg);
            }
            f.write(')');
            break;
        }
        case Expr::Function: {
            auto function = (FunValueExpr*)expr;
            f.write("function"_v);
            writeArgs(f, function->args);
            f.space();
            writeBody(f, function->body);
            break;
        }
    }
}

void writeStmt(Format& f, JsPtr<Stmt> pointer);

void writeBody(Format& f, StmtList body) {
    f.write('{');
    f.newline();
    f.withLevel([&] {
        for(auto stmt: body.contents(f.base)) writeStmt(f, stmt);
    });
    f.startLine();
    f.write('}');
}

void writeStmt(Format& f, JsPtr<Stmt> pointer) {
    auto stmt = f.base[pointer];

    switch(stmt->kind) {
        case Stmt::Block:
            f.startLine();
            writeBody(f, ((BlockStmt*)stmt)->body);
            f.newline();
            break;
        case Stmt::Expression:
            f.startLine();
            writeExpr(f, ((ExprStmt*)stmt)->value);
            f.write(';');
            f.newline();
            break;
        case Stmt::If: {
            auto branch = (IfStmt*)stmt;
            auto then = branch->then;
            auto otherwise = branch->otherwise;

            f.startLine();
            f.write("if"_v);
            f.space();
            f.write('(');
            writeExpr(f, branch->cond);
            f.write(')');
            f.space();
            writeBody(f, then);

            if(otherwise.list.count) {
                f.space();
                f.write("else"_v);
                f.space();
                writeBody(f, otherwise);
            }

            f.newline();
            break;
        }
        case Stmt::Forever:
            f.startLine();
            f.write("for"_v);
            f.space();
            f.write("(;;)"_v);
            f.space();
            writeBody(f, ((ForeverStmt*)stmt)->body);
            f.newline();
            break;
        case Stmt::Break:
            f.startLine();
            f.write("break "_v);
            f.name(((BreakStmt*)stmt)->label);
            f.write(';');
            f.newline();
            break;
        case Stmt::Continue:
            f.startLine();
            f.write("continue "_v);
            f.name(((ContinueStmt*)stmt)->label);
            f.write(';');
            f.newline();
            break;
        case Stmt::Labelled: {
            auto labelled = (LabelledStmt*)stmt;
            f.startLine();
            f.name(labelled->label);
            f.write(':');
            f.space();

            // The label already opened the line, so the statement it introduces continues it rather
            // than indenting again. Its own body still nests at this level.
            f.sameLine = true;
            writeStmt(f, labelled->content);
            break;
        }
        case Stmt::Return: {
            auto returned = (ReturnStmt*)stmt;
            f.startLine();

            if(returned->value) {
                f.write("return "_v);
                writeExpr(f, returned->value);
            } else {
                f.write("return"_v);
            }

            f.write(';');
            f.newline();
            break;
        }
        case Stmt::Decl: {
            auto decl = (DeclStmt*)stmt;
            f.startLine();
            f.write(decl->constant ? "const "_v : "var "_v);
            f.name(decl->name);

            if(decl->value) {
                f.space();
                f.write('=');
                f.space();
                writeExpr(f, decl->value);
            }

            f.write(';');
            f.newline();
            break;
        }
        case Stmt::Fun: {
            auto fun = (FunStmt*)stmt;

            f.startLine();
            f.write("function "_v);
            f.name(fun->name);
            writeArgs(f, fun->args);
            f.space();
            writeBody(f, fun->body);
            f.newline();
            break;
        }
        case Stmt::Comment: {
            if(f.minify) break;
            auto text = f.context.findName(((CommentStmt*)stmt)->text);
            f.startLine();
            f.write("// "_v);
            f.write(stringView(text));
            f.newline();
            break;
        }
    }
}

} // namespace

void formatFile(Net::Writer& writer, Context& context, File& file, bool minify) {
    Format format { writer, context, *file.arena, minify };

    // Strict mode, always: `with` and a few other things this tree cannot build are the reason,
    // and a file that opts in once is one fewer thing for every emitted function to be careful of.
    writer.writeString(minify ? "\"use strict\";"_v : "\"use strict\";\n"_v);

    /*
     * A blank line in front of each function, and in front of the comment that introduces a run of
     * them. Only where the file is meant to be read: minified output has no lines to separate, and
     * §3.6 wants the unminified form to be something a person can debug.
     */
    auto previous = Stmt::Comment;
    auto first = true;

    for(auto stmt: file.statements.contents(format.base)) {
        auto kind = format.base[stmt]->kind;
        auto separated = kind == Stmt::Fun || kind == Stmt::Comment;

        if(separated && !first && previous != Stmt::Comment) format.newline();

        writeStmt(format, stmt);
        previous = kind;
        first = false;
    }
}

} // namespace js
