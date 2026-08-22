#include "parser.h"

#define makeExpr(k, field, contents, location) \
    ast::Expr { .field = contents, .source = context.addLocation(location), .kind = (ast::Expr::Kind)(ast::Expr::k) }

#define makeType(k, field, contents, location, attr) \
    ast::Type { .field = contents, .attributes = attr, .source = context.addLocation(location), .kind = ast::Type::k }

Parser::Parser(Context& context, Lexer& lexer, StringId moduleName):
    BasicParser<Lexer, Token>(context.diagnostics, lexer, moduleName), context(context), arena(2 * 1024 * 1024)
{
    qualifiedId = Context::nameHash("qualified", 9);
    hidingId = Context::nameHash("hiding", 6);
    fromId = Context::nameHash("from", 4);
    asId = Context::nameHash("as", 2);
    checkedRefId = Context::nameHash("*", 1);
    rawPtrId = Context::nameHash("%", 1);
    downtoId = Context::nameHash("downto", 6);
    stepId = Context::nameHash("step", 4);
    arraySizeId = Context::nameHash("*", 1);
    lazyId = Context::nameHash("lazy", 4);

    // A completion request names one module and one byte of it; every other module in the program
    // is parsed exactly as it always was. See Implementation-Tooling.md §8.2.
    if(context.cursor.isSet() && context.cursor.module == moduleName) {
        hasCursor = true;
        cursorOffset = context.cursor.offset;
        cursorId = cursorName(context);
    }

    eat();
}

ast::Expr Parser::emitCursor(Location node, U32 prefixStart) {
    auto expr = makeExpr(Var, var, cursorId, node);

    context.cursor.sentinel = expr.source;
    context.cursor.prefixStart = prefixStart;
    return expr;
}

ast::Expr Parser::eatCursorToken() {
    auto node = currentNode();
    auto start = token.startOffset;

    eat();
    return emitCursor(node, start);
}

StringId Parser::eatCursorName() {
    eatCursorToken();
    return cursorId;
}

ast::Expr Parser::emitCursorHere() {
    // The line and column are the following token's, since they exist for a diagnostic's caret line
    // and the sentinel is never what a diagnostic points at. The offsets are what everything else
    // reads, and both of those are the cursor.
    auto node = currentNode();
    node.sourceStart.offset = cursorOffset;
    node.sourceEnd = node.sourceStart;

    return emitCursor(node, cursorOffset);
}

ast::Module Parser::parseModule() {
    StageScope stage(CompileStage::Parse);

    auto errorCount = context.diagnostics.errorCount();
    auto warningCount = context.diagnostics.warningCount();

    ast::ParseList<ast::Import> imports;
    ast::ParseList<ast::Fixity> ops;
    ast::DeclList decls;

    withLevel([&] {
        sepBy([&] {
            // Skip empty declarations.
            if(token.type == Token::EndOfStmt) return;

            // Parse top-level declarations.
            if(token.type == Token::kwImport) {
                imports.push(arena, parseImport());
            } else if(token.type == Token::kwInfixL || token.type == Token::kwInfixR) {
                ops.push(arena, parseFixity());
            } else {
                auto nextDecl = [&](ast::AttrList attributes) {
                    auto exported = maybe(Token::kwPub).isJust();
                    parseDecl(decls, ::move(attributes), exported);
                };

                ast::AttrList attributes;
                parseAttributes(attributes, false);

                if(attributes.isNotEmpty() && maybe(Token::opColon)) {
                    block([&] {
                        sepBy([&] {
                            ast::AttrList localAttributes;
                            if(attributes.isNotEmpty()) {
                                localAttributes.reserve(arena, attributes.size());
                                for(auto a: attributes.contents(*arena)) localAttributes.push(arena, a);
                            }

                            parseAttributes(localAttributes, false);
                            nextDecl(::move(localAttributes));
                        }, Token::EndOfStmt, Token::EndOfBlock);
                    }, "expected declarations under this attribute group"_v);
                } else {
                    nextDecl(::move(attributes));
                }
            }

            if(!atBlockEnd()) {
                // The previous declaration did not parse all tokens. Skip ahead until we are at
                // the root level again, then continue parsing: one broken declaration should not
                // cost the file the forty that follow it.
                error("expected declaration end"_v);
                eat();
                syncStmt();
            }
        }, Token::EndOfStmt, Token::EndOfFile);
    });

    expect(Token::EndOfFile, "expected file end"_v);

    return ast::Module {
        .region = ::move(arena),
        .name = moduleName,
        .imports = imports,
        .decls = decls,
        .ops = ops,
        .errorCount = context.diagnostics.errorCount() - errorCount,
        .warningCount = context.diagnostics.warningCount() - warningCount,
    };
}

ast::Import Parser::parseImport() {
    ast::Import import {};
    WithLocation location(*this);

    tryMaybe(expect(Token::kwImport, "expected module import"_v), return import);

    import.qualified = maybeVar(qualifiedId).isJust();
    import.from = expectVarOrCon().from({ .id = StringId() }).id;

    maybeParens([&] {
        sepBy([&] {
            auto include = tryMaybe(expectVarOrCon(), return);
            import.include.push(arena, include.id);
        }, Token::Comma, Token::ParenR);
    });

    if(maybeVar(hidingId)) {
        parens([&] {
            sepBy([&] {
                auto exclude = tryMaybe(expectVarOrCon(), return);
                import.exclude.push(arena, exclude.id);
            }, Token::Comma, Token::ParenR);
        });
    }

    if(maybeVar(asId)) {
        auto asName = tryMaybe(expect(Token::ConID, "expected identifier"_v), return import);
        import.localName = asName.id;
    }

    import.source = context.addLocation(location);
    return import;
}

ast::Fixity Parser::parseFixity() {
    ast::Fixity fixity {};
    WithLocation location(*this);

    if(maybe(Token::kwInfixR)) {
        fixity.kind = ast::Fixity::Right;
    } else {
        expect("expected operator fixity"_v, [](auto& t) { return t.type == Token::kwInfixL; });
        fixity.kind = ast::Fixity::Left;
    }

    fixity.precedence = expect(Token::Integer, "expected operator precedence"_v).from({ .integer = 9 }).integer;
    fixity.op = parseQop().var;
    fixity.source = context.addLocation(location);

    return fixity;
}

bool Parser::expectClose(Token::Type end, StringView errorText) {
    if(token.type == end) {
        eat();
        return true;
    }

    /*
     * The construct is being given up on, so the layout suppression it opened is given up on too.
     *
     * A bracket suppresses layout tokens for as long as it is open (Analysis-Language.md §6), and a
     * bracket that is never closed would suppress them to the end of the file - which is where a
     * declaration that failed to parse would take every declaration after it. That is exactly what
     * skipToClose's rule refuses to do: "a delimiter that was never closed should cost the
     * declaration it is in and not the ones after it". It cannot hold that line without a statement
     * boundary to stop at, and this is what puts one back.
     *
     * Before the error rather than after it, so that what the reader is pointed at is the end of the
     * statement the delimiter ran past rather than the first token of the next one.
     */
    lexer.abandonBrackets(token);
    error(errorText);

    return skipToClose(end);
}

bool Parser::skipToClose(Token::Type end) {
    U32 depth = 0;

    while(true) {
        auto type = token.type;

        // The layout tokens are the safe locations to give up at: everything after one of them was
        // written as a new statement, and reading it as part of this construct only loses more.
        if(type == Token::EndOfFile || type == Token::EndOfStmt || type == Token::EndOfBlock) {
            return false;
        }

        if(type == Token::ParenL || type == Token::BracketL || type == Token::BraceL) {
            depth++;
        } else if(type == Token::ParenR || type == Token::BracketR || type == Token::BraceR) {
            if(depth == 0) {
                // A closing token that is not the one we want closes something we are nested in,
                // so it is left for that construct to consume.
                if(type != end) return false;

                eat();
                return true;
            }

            depth--;
        }

        eat();
    }
}

void Parser::parseDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    if(token.type == Token::kwAlias) {
        return parseTypeDecl(decls, ::move(attributes), exported);
    } else if(token.type == Token::kwData) {
        return parseDataDecl(decls, ::move(attributes), exported);
    } else if(token.type == Token::kwForeign) {
        return parseForeignDecl(decls, ::move(attributes), exported);
    } else if(token.type == Token::kwFn || token.type == Token::kwLens || token.type == Token::kwIter) {
        return parseFunDecl(decls, ::move(attributes), exported, !allowSignatures);
    } else if(token.type == Token::kwClass) {
        return parseTraitDecl(decls, ::move(attributes), exported);
    } else if(token.type == Token::kwInstance) {
        return parseInstanceDecl(decls, ::move(attributes), exported);
    } else if(token.type == Token::kwAtData) {
        return parseAttrDecl(decls, ::move(attributes), exported);
    } else {
        auto expr = parseExpr();

        if(ast::isTerminating(expr)) {
            error("terminating statements cannot be used in a global scope"_v, expr.source);
        }

        decls.push(arena, ast::Decl {
            .stmt = expr,
            .attributes = ::move(attributes),
            .source = expr.source,
            .kind = ast::Decl::Stmt,
            .exported = exported,
        });
    }
}

void Parser::parseFunDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported, bool requireBody) {
    WithLocation location(*this);

    auto funKind = parseFunKind();
    expect(Token::kwFn, "expected 'fn'"_v);

    ast::ConstraintList constraints;
    if(token.type == Token::ParenL) {
        parseConstraints(constraints);
    }

    // Without a name there is nothing to declare, and every production after this one would
    // report its own piece of what is missing - a `fn` on a line of its own is one keystroke, not
    // a missing name and a missing argument list and a missing body. So this is the other place
    // the recovering expect belongs: the rest of the line goes, and an error node holds the spot.
    auto parsedName = expectSync("expected function name"_v, stmtStops(), [&](Token& t) {
        return t.type == Token::VarID || t.type == Token::VarSym;
    });

    if(!parsedName) {
        decls.push(arena, ast::Decl {
            .errorName = StringId(),
            .attributes = ::move(attributes),
            .source = context.addLocation(location),
            .kind = ast::Decl::Error,
            .exported = exported,
        });
        return;
    }

    auto name = parsedName.unwrap().id;
    ast::ParseList<ast::Arg> args;

    auto argsClosed = parens([&] {
        sepBy([&] {
            parseArg(args, false);
        }, Token::Comma, Token::ParenR);
    });

    ast::ParsePtr<ast::Type> ret = nullptr;
    auto retBind = ast::BindType::Borrow;

    if(maybe(Token::opArrowR)) {
        retBind = parseResultBind();
        ret = heap(parseType());
    }

    auto source = context.addLocation(location);
    auto implicitReturn = false;

    ast::ParsePtr<ast::Expr> body = nullptr;
    if(maybe(Token::opEquals)) {
        implicitReturn = true;
        WithLocation bodyLocation(*this);

        auto contents = parseExpr();

        if(token.type == Token::kwWhere) {
            auto line = token.endLine;
            eat();

            ast::ParseList<ast::Expr> locals;
            locals.push(arena, contents);
            locals.push(arena, parseVarDecl(bodyLocation, line));

            body = heap(makeExpr(Multi, multi, locals, bodyLocation));
        } else {
            body = heap(contents);
        }
    } else if(token.type == Token::opBar) {
        implicitReturn = true;
        ast::ParseList<ast::Alt> alts;

        withLevel([&] {
            sepBy1([&] {
                expect(Token::opBar, "expected '|' alternative"_v);
                parseAlt(alts);
            }, Token::EndOfStmt);
        });

        Maybe<ast::Expr> pivot;

        if(args.size() >= 2) {
            ast::ParseList<ast::TupArg> pivotArgs;
            pivotArgs.reserve(arena, args.size());

            for(auto a: args.contents(*arena)) {
                pivotArgs.push(arena, ast::TupArg {
                    .name = StringId(),
                    .value = makeExpr(Var, var, a.name, currentNode()),
                });
            }

            pivot = Just(makeExpr(Tup, tup, pivotArgs, currentNode()));
        } else if(args.size() == 1) {
            pivot = Just(makeExpr(Var, var, args.get(*arena, 0).name, currentNode()));
        } else {
            pivot = Just(makeExpr(Tup, tup, {}, currentNode()));
        }

        body = heap(makeExpr(Match, match, heap(ast::MatchExpr { pivot.unwrap(), alts }), currentNode()));
    } else if(token.type == Token::opColon) {
        body = heap(parseBlock(true));
    } else if(requireBody && argsClosed) {
        // An argument list that was left open already cost this declaration a diagnostic, and the
        // body is missing because the signature is: reporting it too describes one mistake twice.
        error("expected function body"_v);
    }

    decls.push(arena, ast::Decl {
        .fun = {
            .name = name,
            .constraints = constraints,
            .args = args,
            .ret = ret,
            .retBind = retBind,
            .body = body,
            .implicitReturn = implicitReturn,
            .kind = funKind,
        },
        .attributes = ::move(attributes),
        .source = source,
        .kind = ast::Decl::Fun,
        .exported = exported,
    });
}

void Parser::parseDataDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);
    expect(Token::kwData, "expected 'data'"_v);
    auto qualified = maybeVar(qualifiedId).isJust();

    ast::ConstraintList constraints;
    if(token.type == Token::ParenL) {
        parseConstraints(constraints);
    }

    auto type = parseSimpleType();
    auto source = context.addLocation(location);
    ast::ParseList<ast::Con> cons;

    if(maybe(Token::opEquals)) {
        sepBy1([&] {
            parseCon(cons);
        }, Token::opBar);
    } else if(token.type == Token::BraceL) {
        WithLocation conLocation(*this);
        auto conType = parseTupleType(conLocation, nullptr);
        cons.push(arena, ast::Con { type.name, heap(conType), {}, conType.source });
    } else {
        error("expected '=' or '{' after type name"_v);
        decls.push(arena, ast::Decl {
            .errorName = type.name,
            .attributes = ::move(attributes),
            .source = source,
            .kind = ast::Decl::Error,
            .exported = exported,
        });
        return;
    }

    decls.push(arena, ast::Decl {
        .data = { cons, type, constraints },
        .attributes = ::move(attributes),
        .source = source,
        .kind = ast::Decl::Data,
        .exported = exported,
        .qualified = qualified,
    });
}

/*
 * `alias Name = Type`, and `alias qualified Name = Type` - a newtype.
 *
 * The two differ only in whether the name is transparent. A plain alias *is* its target, so nothing
 * distinguishes the two types anywhere; a qualified one is a distinct type reached only through a
 * constructor of its own name, which is the same thing `qualified` means on a `data` - access has
 * to name the type.
 *
 * This is an `alias` rather than a `data` shorthand because what follows `=` here is a type and
 * only ever a type. Under `data` the same words are a constructor list, and `data Id = Long` and
 * `data Direction = North` are the same tokens meaning different things.
 */
void Parser::parseTypeDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);
    expect(Token::kwAlias, "expected 'alias'"_v);
    auto qualified = maybeVar(qualifiedId).isJust();

    auto name = parseSimpleType();
    expect(Token::opEquals, "expected '='"_v);
    auto type = parseType();

    ast::ParseList<ast::Derive> derives;
    if(token.type == Token::kwDeriving) {
        WithLocation clauseLocation(*this);
        parseDeriving(derives);
        auto clause = context.addLocation(clauseLocation);

        // A plain alias *is* its target, so there is no second type for an instance to be about and
        // a derivation would either duplicate the target's own instance or contradict it. Reported
        // here rather than in the resolver because it is a statement about the two spellings of
        // `alias` and not about any class in the list.
        if(!qualified && derives.isNotEmpty()) {
            error("a plain alias cannot derive - it names the same type as its target, which already has whatever instances it has; write `alias qualified` for a type of its own"_v, clause);
            derives = {};
        }
    }

    decls.push(arena, ast::Decl {
        .alias = { name, type, derives },
        .attributes = ::move(attributes),
        .source = context.addLocation(location),
        .kind = ast::Decl::Alias,
        .exported = exported,
        .qualified = qualified,
    });
}

void Parser::parseForeignDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);

    expect(Token::kwForeign, "expected 'foreign'"_v);
    auto isFun = maybe(Token::kwFn).isJust();
    auto isStringName = token.type == Token::String;

    auto externName = expect("expected identifier"_v, [&](Token& t) {
        return t.type == Token::String || t.type == Token::VarID;
    }).from({ .id = StringId() }).id;

    StringId localName {};
    if(maybeVar(asId)) {
        localName = expect(Token::VarID, "expected identifier"_v).from({ .id = StringId() }).id;
    } else if(isStringName) {
        error("expected 'as' and foreign import name after string identifier"_v);
    }

    // A normal function type looks exactly like a function declaration when directly after the name.
    if(!maybe(Token::opColon) && !isFun) {
        error("expected ':'"_v);
    }

    auto type = parseType();

    StringId from {};
    if(maybeVar(fromId)) {
        from = expect(Token::String, "expected string for imported library name"_v).from({ .id = StringId() }).id;
    }

    decls.push(arena, ast::Decl {
        .foreign = { .externName = externName, .localName = localName, .from = from, .type = type },
        .attributes = ::move(attributes),
        .source = context.addLocation(location),
        .kind = ast::Decl::Foreign,
        .exported = exported,
    });
}

void Parser::parseTraitDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);
    expect(Token::kwClass, "expected 'class'"_v);

    ast::ConstraintList constraints;
    if(token.type == Token::ParenL) {
        parseConstraints(constraints);
    }

    auto type = parseSimpleType(true);
    auto source = context.addLocation(location);
    ast::DeclList funs;

    // A class with no functions is written without the `:` and the block, because there would be
    // nothing to put in them. `class TrivialCopy(a)` is the case: what it asks of a type is a
    // property rather than an operation, so what an instance of it supplies is its own existence.
    if(maybe(Token::opColon)) {
        block([&] {
            sepBy([&] {
                parseFunDecl(funs, {}, false, false);
            }, Token::EndOfStmt, Token::EndOfBlock);
        }, "expected the class functions, indented under the ':'"_v);
    }

    decls.push(arena, ast::Decl {
        .trait = { type, constraints, funs },
        .attributes = ::move(attributes),
        .source = source,
        .kind = ast::Decl::Trait,
        .exported = exported,
    });
}

void Parser::parseInstanceDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);
    expect(Token::kwInstance, "expected 'instance'"_v);

    // An instance head may be written over type variables, and what those variables have to
    // satisfy is written in front of it exactly as a class writes its superclasses:
    // `instance (Eq(a)) Eq(Maybe(a))`.
    ast::ConstraintList constraints;
    if(token.type == Token::ParenL) {
        parseConstraints(constraints);
    }

    auto type = parseType();
    auto hasBody = expect(Token::opColon, "expected ':' after instance declaration"_v).isJust();

    ast::Decl decl {
        .instance = { .type = type, .constraints = constraints, .decls = {} },
        .attributes = ::move(attributes),
        .source = context.addLocation(location),
        .kind = ast::Decl::Instance,
        .exported = exported,
    };

    if(hasBody) {
        block([&] {
            return sepBy([&] {
                parseDecl(decl.instance.decls, {}, false);
            }, Token::EndOfStmt, Token::EndOfBlock);
        }, "expected the instance definitions, indented under the ':'"_v);
    }

    decls.push(arena, decl);
}

void Parser::parseAttrDecl(ast::DeclList& decls, ast::AttrList attributes, bool exported) {
    WithLocation location(*this);
    expect(Token::kwAtData, "expected '@data'"_v);

    auto pushError = [&](StringId name) {
        decls.push(arena, ast::Decl {
            .errorName = name,
            .attributes = ::move(attributes),
            .source = context.addLocation(location),
            .kind = ast::Decl::Error,
            .exported = exported,
        });
    };

    auto name = tryMaybe(expectVarOrCon("expected identifier"_v), { pushError(StringId()); return; }).id;

    tryMaybe(expect("expected attribute type"_v, [&](Token& t) {
        return t.type == Token::ParenL || t.type == Token::BraceL || t.type == Token::BracketL;
    }), { pushError(name); return; });

    auto type = parseType();

    decls.push(arena, ast::Decl {
        .attr = { name, type },
        .attributes = ::move(attributes),
        .source = context.addLocation(location),
        .kind = ast::Decl::Attr,
        .exported = exported,
    });
}

void Parser::parseCon(ast::ParseList<ast::Con>& list) {
    ast::AttrList attributes;
    parseAttributes(attributes, true);

    /*
     * con	→	conid(type)
     *      |   conid tuptype
     *      |   conid
     */
    WithLocation location(*this);
    auto name = expect(Token::ConID, "expected constructor name"_v).from({ .id = StringId() }).id;
    Maybe<ast::Type> type;

    if(token.type == Token::ParenL) {
        parens([&] {
            type = Just(parseType());
        });
    } else if(token.type == Token::BraceL) {
        type = Just(parseTupleType(location, nullptr));
    } else if(token.type == Token::BracketL) {
        type = Just(parseAType(location, nullptr));
    }

    list.push(arena, ast::Con {
        .name = name,
        .content = type ? heap(type.unwrap()) : nullptr,
        .attributes = attributes,
        .source = context.addLocation(location),
    });
}

ast::Expr Parser::parseBlock(bool isFun) {
    // To make the code more readable we avoid using '=' inside expressions, and use '->' instead.
    if(maybe(isFun ? Token::opEquals : Token::opArrowR)) {
        return parseExpr();
    }

    // A block whose ':' is missing is not looked for at all. Reading one anyway would report the
    // ':' and then the block, two messages for one mistake, and this is the shape a function whose
    // signature is still being typed is in.
    WithLocation location(*this);
    if(!expect(Token::opColon, "expected ':'"_v)) return makeExpr(Error, var, StringId(), location);

    Maybe<ast::Expr> expr;
    block([&] {
        expr = Just(parseExprSeq());
    }, "expected an indented block after the ':'"_v);

    return expr ? expr.unwrap() : makeExpr(Error, var, StringId(), location);
}

ast::Expr Parser::parseExprSeq() {
    /*
     * exprseq	→	expr
     * 			|	expr0; …; exprn	(statements, n ≥ 2)
     */
    WithLocation location(*this);
    ast::ParseList<ast::Expr> exprs;

    /*
     * A statement the author has started nothing of - §8.2.
     *
     * An empty line produces no token at all: the lexer skips the whitespace and the layout token
     * lands on whatever comes next, so no production runs at the cursor and no other site here can
     * catch it. That is the state a body is in the moment a line is opened to write in, which makes
     * it one of the positions completion is asked for most.
     *
     * Checked at each statement boundary rather than only at the end, because where in the block the
     * cursor is decides what is in scope: a binding written *below* the empty line is not something
     * a name there can refer to.
     *
     * Innermost first, since a nested block finishes before the one containing it - so the deepest
     * scope that could hold the cursor claims it, and the outer ones then see it already emitted.
     */
    auto atEmptyStatement = [&] {
        if(atCursorGap()) exprs.push(arena, emitCursorHere());
    };

    sepBy1([&] {
        atEmptyStatement();
        exprs.push(arena, parseTypedExpr());
    }, Token::EndOfStmt);

    atEmptyStatement();

    if(exprs.size() > 1) {
        return makeExpr(Multi, multi, exprs, location);
    } else if(exprs.size() == 1) {
        return exprs.get(*arena, 0);
    } else {
        return makeExpr(Error, var, StringId(), location);
    }
}

ast::Expr Parser::parseExpr() {
    return parseTypedExpr();
}

ast::Expr Parser::parseTypedExpr() {
    /*
     * typedexpr	→	infixexpr :: type
     *				|	infixexpr
     */

    WithLocation location(*this);
    auto expr = parseInfixExpr(location);

    if(maybe(Token::opColonColon)) {
        auto type = parseType();
        return makeExpr(Coerce, coerce, heap(ast::CoerceExpr { .target = expr, .type = type }), location);
    } else {
        return expr;
    }
}

ast::Expr Parser::parseInfixExpr(const WithLocation& location) {
    /*
     * infixexp		→	pexp qop infixexp			(infix operator application)
     * 				|	pexp = infixexp				(assignment)
     *				|	pexp
     */
    auto lhs = parsePrefixExpr(location);

    // `is` binds one operand of the chain rather than the chain itself, which is what makes
    // `p is Just(v) && v > 0` mean what it looks like: the `is` test is `&&`'s left operand, and
    // the precedence pass sees an ordinary operand. A pattern cannot begin with an operator, so
    // there is nothing to look ahead for. Testing the result of an infix expression needs parens
    // (`(a + b) is p`), since fixity is resolved long after this.
    if(maybe(Token::kwIs)) {
        auto pat = parsePattern();
        lhs = makeExpr(Is, is, heap(ast::IsExpr { .value = lhs, .pat = pat }), location);
    }

    if(maybe(Token::opEquals)) {
        auto rhs = parseExpr();
        return makeExpr(Assign, assign, heap(ast::AssignExpr { .target = lhs, .value = rhs }), location);
    } else if(token.type == Token::VarSym || token.type == Token::Grave) {
        // Binary operator.
        auto op = parseQop();

        WithLocation rhsLocation(*this);
        auto rhs = parseInfixExpr(rhsLocation);
        return makeExpr(Infix, infix, heap(ast::InfixExpr { .lhs = lhs, .rhs = rhs, .op = op }), location);
    } else {
        return lhs;
    }
}

ast::Expr Parser::parsePrefixExpr(const WithLocation& location) {
    /*
     * pexp		→	varsym lexp				(prefix operator application)
     *			|	lexp
     */
    if(auto sym = maybe(Token::VarSym)) {
        auto op = makeExpr(Var, var, sym.unwrap().id, location);

        WithLocation onLocation(*this);
        auto expr = parsePrefixExpr(onLocation);

        return makeExpr(Prefix, prefix, heap(ast::PrefixExpr { .on = expr, .op = op }), location);
    } else {
        return parseLeftExpr(location);
    }
}

ast::Expr Parser::parseLeftExpr(const WithLocation& location) {
    if(token.type == Token::kwLet) {
        auto l = token.endLine;
        eat();
        return parseVarDecl(location, l);
    } else if(token.type == Token::kwMatch) {
        return parseMatchExpr(location);
    } else if(token.type == Token::kwIf) {
        return parseIfExpr();
    } else if(maybe(Token::kwWhile)) {
        auto cond = parseExpr();
        auto source = location;
        auto loop = parseBlock(false);
        return makeExpr(While, whileLoop, heap(ast::WhileExpr { .cond = cond, .body = loop }), source);
    } else if(maybe(Token::kwFor)) {
        auto pat = parsePattern();
        expect(Token::kwIn, "expected 'in'"_v);

        WithLocation fromLocation(*this);
        auto from = parseChainExpr(fromLocation);

        ast::ParsePtr<ast::Expr> step = nullptr, to = nullptr;
        bool reverse = false;
        bool inclusive = false;
        bool hasTo = false;

        if(token.type == Token::opDotDot) {
            eat();
            hasTo = true;
        } else if(token.type == Token::opDotDotEq) {
            eat();
            inclusive = true;
            hasTo = true;
        } else if(token.type == Token::VarID && token.data.id == downtoId) {
            eat();
            reverse = true;
            hasTo = true;
        }

        if(hasTo) {
            WithLocation toLocation(*this);
            to = heap(parseChainExpr(toLocation));
        }

        if(maybeVar(stepId)) {
            WithLocation stepLocation(*this);
            step = heap(parseChainExpr(stepLocation));
        }

        auto body = parseBlock(false);
        return makeExpr(For, forLoop, heap(ast::ForExpr { pat, from, body, to, step, reverse, inclusive }), location);
    } else if(maybe(Token::kwReturn)) {
        Maybe<ast::Expr> body;
        if(token.type != Token::EndOfStmt && token.type != Token::EndOfBlock) body = Just(parseExpr());

        return makeExpr(Ret, ret, body ? heap(body.unwrap()) : nullptr, location);
    } else if(maybe(Token::kwYield)) {
        auto value = parseExpr();
        return makeExpr(Yield, yield, heap(value), location);
    } else if(maybe(Token::kwBreak)) {
        Maybe<ast::Expr> body;
        if(token.type != Token::EndOfStmt && token.type != Token::EndOfBlock) body = Just(parseExpr());

        return makeExpr(Break, breakValue, body ? heap(body.unwrap()) : nullptr, location);
    } else if(maybe(Token::kwContinue)) {
        return makeExpr(Continue, var, StringId(), location);
    } else {
        return parseAppExpr();
    }
}

ast::Expr Parser::parseAppExpr() {
    WithLocation location(*this);
    auto base = parseBaseExpr();
    return parseChain(base, location);
}

ast::Expr Parser::parseChain(ast::Expr base, const WithLocation& startLocation) {
    if(token.type == Token::ParenL) {
        ast::ParseList<ast::TupArg> args;

        parens([&] {
            sepBy([&] {
                parseTupArg(args);
            }, Token::Comma, Token::ParenR);
        });

        return parseChain(makeExpr(App, app, heap(ast::AppExpr { .callee = base, .args = args }), startLocation), startLocation);
    } else if(token.type == Token::BracketL) {
        ast::ParseList<ast::TupArg> args;

        brackets([&] {
            sepBy([&] {
                /*
                 * `xs[a..b]` - Implementation-Containers.md §4's subslice.
                 *
                 * One argument still, and the range is that argument: an index and a range are the
                 * two things a subscript can be given, and telling them apart here rather than by
                 * counting arguments is what keeps `xs[a, b]` an error instead of a second spelling
                 * of the same thing.
                 */
                WithLocation rangeLocation(*this);
                auto index = parseExpr();

                if(maybe(Token::opDotDot)) {
                    auto to = parseExpr();
                    args.push(arena, ast::TupArg { StringId(), makeExpr(Range, range,
                        heap(ast::RangeExpr { index, to, false }), rangeLocation) });
                    return;
                }

                args.push(arena, ast::TupArg { StringId(), index });
            }, Token::Comma, Token::BracketR);
        });

        return parseChain(makeExpr(Sub, sub, heap(ast::AppExpr { .callee = base, .args = args }), startLocation), startLocation);
    } else if(maybe(Token::opDot)) {
        WithLocation location(*this);
        auto app = parseSelExpr(location);
        return parseChain(makeExpr(Field, field, heap(ast::FieldExpr { .target = base, .field = app }), startLocation), startLocation);
    } else if(auto optional = maybeNode(Token::opQuestionDot)) {
        /*
         * `?.` - optional chaining, and a different operator from `?` rather than a spelling of it.
         * `?` leaves the enclosing function; this skips the rest of *this chain* and produces the
         * carrier's empty case, which is what every language with the spelling means by it.
         *
         * What is built is the *unwrapping* on its own, with the suffix after it left to the loop
         * below - so `a?.b` is a field of an unwrap, `a?.[i]` a subscript of one and `a?.(x)` a
         * call of one. Three spellings, one node, and nothing below the parser has to know which
         * of them was written.
         *
         * The location is the `?.` itself. Everything this node can be wrong about is about the
         * operator - a carrier with no `Try` instance, a skip that cannot be rebuilt - and a span
         * covering the whole chain would point at the parts that are fine.
         */
        auto unwrapped = makeExpr(Unwrap, unwrap, heap(base), optional.unwrap().node);

        // A bracket is the suffix, and the chain loop already reads both. Anything else is the
        // field form, whose name is read here so that `a?.` with nothing after it is one error
        // about a missing name rather than a chain that quietly ended.
        if(token.type == Token::ParenL || token.type == Token::BracketL) {
            return parseChain(unwrapped, startLocation);
        }

        WithLocation fieldLocation(*this);
        auto app = parseSelExpr(fieldLocation);

        return parseChain(makeExpr(Field, field, heap(ast::FieldExpr { .target = unwrapped, .field = app }),
                                   startLocation), startLocation);
    } else if(auto question = maybeNode(Token::opQuestion)) {
        /*
         * `x?` - Implementation-Semantics.md part 5's early exit.
         *
         * A suffix of the chain rather than a form of its own, so it binds tighter than every infix
         * operator and looser than nothing: `parse(a)? + parse(b)?` unwraps each call and adds the
         * two payloads, and `xs.get(i)?.name` reads a field of what came out. That is the same
         * property the other three suffixes have - each is closed by its own token - which is what
         * lets it be written in a `for` header too.
         *
         * The location is the `?` itself, not the chain it ends. Everything this operator can be
         * wrong about is about the operator - a carrier with no `Try` instance, an enclosing
         * signature that cannot be exited through - so pointing at the operand would name the one
         * part of `parse(text)?` that is fine.
         */
        return parseChain(makeExpr(Try, tryValue, heap(base), question.unwrap().node), startLocation);
    } else {
        return base;
    }
}

ast::Expr Parser::parseMatchExpr(const WithLocation& location) {
    expect(Token::kwMatch, "expected 'match'"_v);

    auto pivot = parseExpr();
    auto hasAlts = expect(Token::opColon, "expected ':' after match-expression"_v).isJust();
    Location source(location);

    // A match with no alternatives is kept rather than replaced by an error node: the pivot is
    // real, and an editor asked about it while the first alternative is still being typed should
    // get the answer. Nothing downstream reports it again - see resolveMatch.
    ast::ParseList<ast::Alt> alts;
    if(hasAlts) {
        block([&] {
            sepBy1([&] {
                parseAlt(alts);
            }, Token::EndOfStmt);
        }, "expected the match alternatives, indented under the ':'"_v);
    }

    return makeExpr(Match, match, heap(ast::MatchExpr { pivot, alts }), source);
}

ast::Expr Parser::parseIfExpr() {
    WithLocation location(*this);
    expect(Token::kwIf, "expected 'if'"_v);

    if(maybe(Token::opColon)) {
        Maybe<ast::Expr> cond;
        ast::ParseList<ast::IfCase> cases;

        // Multi-way if.
        block([&] {
            sepBy1([&] {
                if(token.type == Token::kw_ || token.type == Token::kwElse) {
                    auto source = currentNode();
                    eat();
                    cond = Just(makeExpr(Lit + ast::Literal::Bool, lit, { .b = true }, source));
                } else {
                    cond = Just(parseExpr());
                }

                expect(Token::opArrowR, "expected '->' after if condition"_v);
                auto then = parseExpr();
                cases.push(arena, ast::IfCase { .cond = cond.unwrap(), .then = then });
            }, Token::EndOfStmt);
        }, "expected the if cases, indented under the ':'"_v);

        return makeExpr(MultiIf, multiIf, cases, location);
    } else {
        auto cond = parseExpr();
        Maybe<ast::Expr> then, otherwise;

        if(token.type == Token::opColon) {
            then = Just(parseBlock(false));
        } else {
            expect(Token::kwThen, "expected 'then' after if-expression"_v);
            then = Just(parseExpr());
        }

        // We should only eat the statement end if there is an else after.
        SaveLexer save(lexer);
        auto savedToken = token;

        if(token.type == Token::EndOfStmt) eat();

        if(maybe(Token::kwElse)) {
            otherwise = Just(token.type == Token::opColon ? parseBlock(false) : parseExpr());
        } else {
            save.restore();
            token = savedToken;
        }

        return makeExpr(If, singleIf, heap(ast::IfExpr { cond, then.unwrap(), otherwise }), location);
    }
}

ast::Expr Parser::parseBaseExpr() {
    WithLocation location(*this);

    // A `ConID` never reaches parseSelExpr - it is a constructor or a type here, not a selection -
    // so the cursor in one has to be caught before that branch runs. §8.1's first kind is types and
    // constructors as much as it is functions, and `Ma|` should offer `Maybe` and `Just` alike.
    if(atCursorName()) return eatCursorToken();

    auto funKind = parseFunKind();
    auto hasParen = maybe(Token::ParenL).isJust();

    if(!hasParen && funKind != ast::FunKind::Plain) {
        expect(Token::ParenL, "expected '(' after 'lens'/'iter'"_v);
        hasParen = true;
    }

    if(hasParen) {
        // Cases to handle:
        // () block
        // (expr)
        // (varexpr) block
        // (varexpr: type) block
        // (varexpr, ...) block
        // (varexpr: type, ...) block
        if(maybe(Token::ParenR)) {
            // () block
            return makeExpr(Fun, fun, heap(ast::FunExpr { .body = parseBlock(false), .kind = funKind }), location);
        }

        if(token.type == Token::opArrowR || token.type == Token::opAmp) {
            ast::ParseList<ast::Arg> args;
            sepBy1([&] {
                parseArg(args, false);
            }, Token::Comma);

            // If the list was never closed, the lambda's body is not what follows it - parsing
            // one here is what makes an unclosed '(' swallow the declarations after it.
            if(!expectClose(Token::ParenR, "expected ',' or ')' in argument list"_v)) {
                return makeExpr(Error, var, StringId(), location);
            }

            return makeExpr(Fun, fun, heap(ast::FunExpr {
                args,
                parseBlock(false),
                funKind,
            }), location);
        }

        auto expr = parseExpr();

        if(maybe(Token::ParenR)) {
            // Cases to handle:
            // (expr)
            // (varexpr) block
            if(expr.kind == ast::Expr::Var && (token.type == Token::opColon || token.type == Token::opArrowR)) {
                ast::ParseList<ast::Arg> args;
                args.push(arena, ast::Arg { .source = expr.source, .name = expr.var, .type = nullptr, .def = nullptr });

                return makeExpr(Fun, fun, heap(ast::FunExpr { args, parseBlock(false), funKind }), location);
            } else if(funKind != ast::FunKind::Plain) {
                error("expected a lambda argument list after 'lens'/'iter'"_v);
                return makeExpr(Fun, fun, heap(ast::FunExpr { .body = expr, .kind = funKind }), location);
            } else {
                return makeExpr(Nested, nested, heap(expr), location);
            }
        }

        // None of the remaining cases can continue at the end of a statement or block, so what
        // this is is a parenthesis that was never closed. Reporting that and keeping the
        // expression is both the better diagnostic and the cheaper recovery: reading on as an
        // argument list would take the following declarations for this lambda's body.
        //
        // `suppressedLayout` is the same end of statement with the `(` this branch is inside still
        // counted against it - see delimited(), which has the same pair for the same reason. This
        // one is written out rather than delegated because the recovery differs: the expression
        // already read is worth keeping, and there is no closing token to skip to.
        if(atBlockEnd() || token.suppressedLayout) {
            lexer.abandonBrackets(token);
            error("expected ')'"_v);
            return makeExpr(Nested, nested, heap(expr), location);
        }

        // Cases to handle:
        // (varexpr: type) block
        // (varexpr: type, ...) block
        // (varexpr, ...) block
        auto firstName = expr.kind == ast::Expr::Var ? expr.var : StringId();
        if(!firstName) error("expected argument name"_v);

        Maybe<ast::Type> firstType;
        if(maybe(Token::opColon)) {
            firstType = Just(parseType());
        }

        ast::ParseList<ast::Arg> args;
        args.push(arena, ast::Arg {
            .source = context.addLocation(location),
            .name = firstName,
            .type = firstType ? heap(firstType.unwrap()) : nullptr,
            .def = nullptr,
        });

        if(maybe(Token::Comma)) {
            sepBy1([&] {
                parseArg(args, false);
            }, Token::Comma);
        }

        if(!expectClose(Token::ParenR, "expected ',' or ')' in argument list"_v)) {
            return makeExpr(Error, var, StringId(), location);
        }

        return makeExpr(Fun, fun, heap(ast::FunExpr { args, parseBlock(false), funKind }), location);
    } else if(token.type == Token::BraceL) {
        return parseTupleExpr(location);
    } else if(token.type == Token::BracketL) {
        return parseArrayExpr(location);
    } else if(auto con = maybe(Token::ConID)) {
        auto type = ast::Type {
            .name = con.unwrap().id,
            .attributes = nullptr,
            .source = context.addLocation(location),
            .kind = ast::Type::Con,
        };

        if(token.type == Token::ParenL) {
            ast::ParseList<ast::TupArg> args;

            parens([&] {
                sepBy([&] {
                    parseTupArg(args);
                }, Token::Comma, Token::ParenR);
            });

            return makeExpr(Con, con, heap(ast::ConExpr { .type = type, .args = args }), location);
        } else if(token.type == Token::BraceL) {
            auto tup = parseTupleExpr(location);
            return makeExpr(Con, con, heap(ast::ConExpr { .type = type, .args = tup.tup }), location);
        } else {
            return makeExpr(Con, con, heap(ast::ConExpr { .type = type }), location);
        }
    } else {
        return parseSelExpr(location);
    }
}

/*
 * The three slots of a `for` header.
 *
 * A `for` is the one form with keywords *after* an expression - `..`, `downto`, `step` and the
 * closing `:` - so what it takes has to stop before them. That is one property, and it is narrower
 * than `selexpr`: no **top-level infix operator**. A suffix chain has that property, because every
 * one of its suffixes is closed by its own bracket or takes exactly one name, so none of them can
 * reach past the expression to eat a following `..` or `step`.
 *
 * So `for x in upTo(n):` and `for i in 0 .. xs.length():` parse, and `for i in a + 1 .. b` still
 * needs its parentheses. That is a readability rule as much as a parsing one: nothing between the
 * keywords of a header may be an operator chain, so where each slot ends is clear to a reader
 * without knowing any fixity.
 *
 * Not `parseSelExpr` itself, which would change what `a.b(c)` means: the field position after `.`
 * is a `selexpr`, and widening it there would read the call as part of the field name rather than
 * as applied to the field.
 */
ast::Expr Parser::parseChainExpr(const WithLocation& location) {
    return parseChain(parseSelExpr(location), location);
}

ast::Expr Parser::parseSelExpr(const WithLocation& location) {
    // The name the cursor is in - §8.2. This is the field position of a `.` as well as the ordinary
    // one, which is what makes `xs.le|` a member request rather than a scope request.
    if(atCursorName()) return eatCursorToken();

    if(token.type >= Token::FirstLiteral && token.type <= Token::LastLiteral) {
        if(token.type == Token::String) {
            return parseStringExpr(location);
        } else {
            return toLiteral(location);
        }
    } else if(auto v = maybe(Token::VarID)) {
        return makeExpr(Var, var, v.unwrap().id, location);
    } else if(token.type == Token::ParenL) {
        Maybe<ast::Expr> expr;
        parens([&] { expr = Just(parseExpr()); });

        // `parens` reports and gives up without reading anything when the `(` is the last thing on
        // the line - `f(` in the middle of being typed - so there is no inner expression to wrap.
        // The error node stands for the one that has not been written; unwrapping here crashed the
        // parser, and therefore the language server, on every keystroke that left a `(` open.
        if(!expr) return makeExpr(Error, var, StringId(), location);

        return makeExpr(Nested, nested, heap(expr.unwrap()), location);
    } else if(atCursorGap()) {
        /*
         * An expression the author has not started typing - `xs.` at the end of a line, or a bare
         * `.` waiting for a field name. The sentinel rather than the error node, and no diagnostic:
         * the cursor is where the text is *being* written, so what is missing there is not a mistake
         * to report.
         */
        return emitCursorHere();
    } else {
        error("expected an expression"_v);
        return makeExpr(Error, var, StringId(), location);
    }
}

ast::Expr Parser::parseStringExpr(const WithLocation& location) {
    auto string = tryMaybe(expect(Token::String, "expected string literal"_v), return makeExpr(Error, var, StringId(), location)).id;

    // Check if the string contains formatting.
    if(token.type == Token::StartOfFormat) {
        ast::ParseList<ast::FormatChunk> chunks;
        chunks.push(arena, { string, nullptr });

        while(maybe(Token::StartOfFormat)) {
            auto expr = parseExpr();
            expect(Token::EndOfFormat, "expected end of string format after this expression"_v);

            auto part = expect(Token::String, "expected string part"_v).from({ .id = StringId() }).id;
            chunks.push(arena, ast::FormatChunk { part, heap(expr) });
        }

        return makeExpr(Format, format, chunks, location);
    } else {
        return makeExpr(Lit + ast::Literal::String, lit, ast::Literal { .s = string }, location);
    }
}

ast::Expr Parser::parseVarDecl(const WithLocation& location, U32 line) {
    ast::ParseList<ast::VarDecl> list;

    if(token.startLine == line) {
        list.push(arena, parseDeclExpr());
    } else {
        // Parse one or more declarations, separated as statements.
        block([&] {
            sepBy1([&] {
                list.push(arena, parseDeclExpr());
            }, Token::EndOfStmt);
        }, "expected the bindings, indented under the 'where'"_v);
    }

    return makeExpr(Decl, decl, list, location);
}

ast::VarDecl Parser::parseDeclExpr() {
    // Attributes come first, as they do on a declaration: `@heap let big = ...`.
    ast::AttrList attributes;
    parseAttributes(attributes, true);

    // A `let` takes the same binding conventions as a parameter, written the same way and in
    // the same place - and one a parameter has no use for. `&->` is the two axes at once:
    // move out of a place that has another name, into storage this binding may write through.
    // The order is fixed, because two spellings of one thing is one spelling too many.
    auto bind = ast::BindType::Borrow;
    auto sink = false;

    if(maybe(Token::opArrowR)) {
        bind = ast::BindType::Sink;
    } else if(maybe(Token::opAmp)) {
        bind = ast::BindType::Ref;
    } else if(maybe(Token::opAmpArrowR)) {
        bind = ast::BindType::Ref;
        sink = true;
    }

    auto pat = parsePattern();

    if(maybe(Token::opEquals)) {
        auto expr = parseExpr();
        ast::ParseList<ast::Alt> alts;

        if(auto node = maybeNode(Token::opBar)) {
            if(maybe(Token::kwMatch)) {
                if(maybe(Token::opColon)) {
                    auto parsed = block([&] {
                        sepBy1([&] {
                            parseAlt(alts);
                        }, Token::EndOfStmt);
                    }, "expected the match alternatives, indented under the ':'"_v);

                    /*
                     * An alternative list that has not been typed yet is one poisoned alternative
                     * rather than none.
                     *
                     * None is what `let Just(x) = e` with no `|` at all looks like, and the
                     * resolver rightly says a refutable pattern needs alternatives - which is the
                     * one thing this declaration does not need to be told. An `Error` pattern
                     * covers every value and resolves to `Never`, so it silences that report and
                     * the two that would follow it without any of them learning about recovery.
                     */
                    if(!parsed) {
                        auto source = node.unwrap().node;
                        alts.push(arena, ast::Alt {
                            .pat = { .source = context.addLocation(source), .kind = ast::Pat::Error },
                            .expr = makeExpr(Error, var, StringId(), source),
                        });
                    }
                } else {
                    parseAlt(alts);
                }
            } else if(token.type == Token::kw_ || token.type == Token::kwElse) {
                // `| else -> expr` is the wildcard written out. It needs no lookahead to tell
                // from the shorthand below, because neither `_` nor `else` can begin an
                // expression; a fallback that tests something instead needs the `| match:` form.
                parseAlt(alts);
            } else {
                auto e = parseExpr();
                alts.push(arena, { .pat = { .source = context.addLocation(node.unwrap().node), .kind = ast::Pat::Any }, .expr = e });
            }
        }

        ast::ParsePtr<ast::Expr> in = nullptr;
        if(maybe(Token::kwIn)) {
            in = heap(parseExpr());
        }

        return { pat, heap(expr), in, alts, bind, sink, ::move(attributes) };
    } else {
        return { pat, nullptr, nullptr, {}, bind, sink, ::move(attributes) };
    }
}

ast::Expr Parser::parseTupleExpr(const WithLocation& location) {
    expect(Token::BraceL, "expected tuple expression"_v);

    if(maybe(Token::BraceR)) {
        // An empty tuple is equivalent to an expression of unit type.
        return makeExpr(Tup, tup, {}, location);
    }

    auto updateBind = ast::BindType::Borrow;
    if(maybe(Token::opArrowR)) updateBind = ast::BindType::Sink;

    // For non-empty tuples, parse the first value to check for update expressions.
    auto firstQualified = maybe(Token::opTilde).isJust();

    auto first = parseExpr();
    if(firstQualified && first.kind != ast::Expr::Var) {
        error("expected variable name"_v);
    }

    if(maybe(Token::opBar)) {
        ast::TupUpdateExpr update { .value = first, .bind = updateBind };

        sepBy1([&] {
            parseTupUpdateArg(update.args);
        }, Token::Comma);

        expectClose(Token::BraceR, "expected '}' after tuple expression"_v);
        return makeExpr(TupUpdate, tupUpdate, heap(update), location);
    }

    if(updateBind == ast::BindType::Sink) {
        error("expected '|' after move-update source"_v, first.source);
    }

    ast::ParseList<ast::TupArg> args;

    if(maybe(Token::opColon)) {
        if(first.kind != ast::Expr::Var) {
            error("expected name before field contents"_v, first.source);
        }

        args.push(arena, ast::TupArg { first.kind == ast::Expr::Var ? first.var : StringId(), parseExpr() });
    } else {
        args.push(arena, ast::TupArg { StringId(), first });
    }

    if(maybe(Token::Comma)) {
        sepBy1([&] {
            parseTupArg(args);
        }, Token::Comma);
    }

    expectClose(Token::BraceR, "expected '}' after tuple expression"_v);
    return makeExpr(Tup, tup, args, location);
}

ast::Expr Parser::parseArrayExpr(const WithLocation& location) {
    expect(Token::BracketL, "expected array expression"_v);

    if(maybe(Token::BracketR)) {
        return makeExpr(Array, arr, {}, location);
    } else if(maybe(Token::opColon)) {
        expectClose(Token::BracketR, "expected ']' after empty map"_v);
        return makeExpr(Map, map, {}, location);
    }

    auto first = parseExpr();

    if(maybe(Token::opColon)) {
        auto firstValue = parseExpr();

        ast::ParseList<ast::MapArg> contents;
        contents.push(arena, ast::MapArg { first, firstValue });

        if(maybe(Token::Comma)) {
            sepBy1([&] {
                auto key = parseExpr();
                expect(Token::opColon, "expected ':' after map item key"_v);
                auto value = parseExpr();
                contents.push(arena, ast::MapArg { key, value });
            }, Token::Comma);
        }

        expectClose(Token::BracketR, "expected ']' after map end"_v);
        return makeExpr(Map, map, contents, location);
    } else {
        ast::ParseList<ast::Expr> contents;
        contents.push(arena, first);

        if(maybe(Token::Comma)) {
            sepBy1([&] {
                contents.push(arena, parseExpr());
            }, Token::Comma);
        }

        expectClose(Token::BracketR, "expected ']' after array end"_v);
        return makeExpr(Array, arr, contents, location);
    }
}

void Parser::parseTupArg(ast::ParseList<ast::TupArg>& list) {
    auto qualified = maybe(Token::opTilde).isJust();

    auto arg = parseExpr();

    if(qualified && arg.kind != ast::Expr::Var) {
        error("expected variable name"_v);
    }

    if(maybe(Token::opColon)) {
        if(arg.kind != ast::Expr::Var) {
            error("expected name before field contents"_v, arg.source);
        }

        list.push(arena, { arg.kind == ast::Expr::Var ? arg.var : StringId(), parseExpr() });
    } else if(qualified && arg.kind == ast::Expr::Var) {
        list.push(arena, { arg.var, arg });
    } else {
        list.push(arena, { StringId(), arg });
    }
}

/*
 * One replacement of a tuple update: `field: expr`, `.path.to.field: expr`, or the `~field`
 * shorthand for `field: field`.
 *
 * The path is handed to the resolver as written rather than expanded into nested updates here.
 * Expanding it would name the update's source once per level, which evaluates a source with side
 * effects more than once and makes two paths sharing a prefix - `{v | .a.b: 1, .a.c: 2}` - build
 * two separate copies of `v.a`, the second of which replaces the first. See TupUpdateArg.
 */
void Parser::parseTupUpdateArg(ast::ParseList<ast::TupUpdateArg>& list) {
    WithLocation location(*this);
    ast::ParseList<StringId> path;

    /*
     * A segment of the path the cursor is in - `{v | ori|gin: p}` and `{v | .origin.x|: 1}`. The
     * segment is a name rather than an expression here, so the sentinel is one too.
     *
     * The gap case is `{v | |}`, where the field has not been typed at all. `atCursorGap` is
     * consulted only here, at the point a name was required and is not there, which is the
     * condition its own comment asks for: it is true of a great many positions and means something
     * only where a production has run out.
     */
    auto segment = [&]() -> StringId {
        if(atCursorName() && token.type == Token::VarID) return eatCursorName();
        if(atCursorGap()) {
            emitCursorHere();
            return cursorId;
        }

        auto field = expect(Token::VarID, "expected field name in update path"_v);
        return field ? field.unwrap().id : StringId(0);
    };

    if(maybe(Token::opDot)) {
        sepBy1([&] {
            if(auto field = segment()) path.push(arena, field);
        }, Token::opDot);

        expect(Token::opColon, "expected ':' after update path"_v);
        if(path.isNotEmpty()) list.push(arena, ast::TupUpdateArg { path, parseExpr() });
        return;
    }

    auto shorthand = maybe(Token::opTilde).isJust();

    if((atCursorName() && token.type == Token::VarID) || atCursorGap()) {
        path.push(arena, segment());

        // The replacement is whatever follows a `:`, and an `Error` node where the author has not
        // got that far. Pushed either way, because the path is what the resolver walks to reach the
        // cursor - an argument left out here is a cursor nothing ever reads.
        auto value = maybe(Token::opColon) ? parseExpr() : makeExpr(Error, var, StringId(), location);
        list.push(arena, ast::TupUpdateArg { path, value });
        return;
    }

    auto name = expect(Token::VarID, "expected the name of the field to update"_v);
    if(!name) return;

    path.push(arena, name.unwrap().id);

    // `~x` names the field and its replacement at once, as it does in a tuple construction.
    if(shorthand && token.type != Token::opColon) {
        auto value = ast::Expr {
            .var = name.unwrap().id,
            .source = context.addLocation(location),
            .kind = ast::Expr::Var,
        };

        list.push(arena, ast::TupUpdateArg { path, value });
        return;
    }

    expect(Token::opColon, "expected ':' after the name of the field to update"_v);
    list.push(arena, ast::TupUpdateArg { path, parseExpr() });
}

// The binding conventions are symbols, so no lookahead is needed to tell one from a name:
// `->` and `&` cannot start an identifier. Out-parameters - writing into storage the caller
// owns and has not initialized - are spelled `@uninit &` and belong to the ownership
// milestone alongside `&` parameters themselves; they are not a fourth convention.
/*
 * The convention written on a *result*, which today is only a `lens`/`iter`'s hand-over -
 * Analysis-Language.md §3a.
 *
 * `-> ->T`, and the second arrow is a token of its own rather than part of the first. That is the
 * lexer's rule rather than a choice made here: a symbol run is classified whole, so `->->` is one
 * lexeme and not two, and the space between them is what makes this two arrows. Written into the
 * grammar as two tokens for the same reason.
 *
 * Accepted wherever a result type is, and refused in resolve for a kind that has no hand-over to
 * have a convention about. Reading it here and reporting there is what keeps `fn f() -> ->Int` from
 * being a *parse* error, which would point at the arrow rather than at the missing `iter`.
 */
ast::BindType Parser::parseResultBind() {
    if(maybe(Token::opArrowR)) return ast::BindType::Sink;
    return ast::BindType::Borrow;
}

ast::BindType Parser::parseBindType() {
    if(maybe(Token::opArrowR)) return ast::BindType::Sink;
    if(maybe(Token::opAmp)) return ast::BindType::Ref;

    // The two axes at once. `Ref` here is the use axis alone; the source axis is the caller's to
    // record, because `BindType` has one slot and this is the combination that needs two.
    if(maybe(Token::opAmpArrowR)) return ast::BindType::Ref;

    return ast::BindType::Borrow;
}

// `return` is the return-root marker only in parameter position, where the statement keyword
// cannot appear; it is written before the binding convention (`return &value: T`). Which
// conventions it may combine with, and that a marked parameter cannot also have a default
// value, are resolve-stage rules rather than grammatical ones.
bool Parser::parseReturnRoot() {
    return maybe(Token::kwReturn).isJust();
}

// `@lazy` is written outermost, before the `return` marker and the binding convention. It is the
// one attribute with a meaning in parameter position, so an `@` here is either it or a mistake -
// which is why this consumes the name rather than going through parseAttributes and leaving a list
// nothing downstream would read.
bool Parser::parseLazy() {
    if(token.type != Token::opAt) return false;

    eat();
    if(token.type == Token::VarID && token.data.id == lazyId) {
        eat();
        return true;
    }

    error("`@lazy` is the only attribute a parameter can carry"_v);
    return false;
}

void Parser::parseArg(ast::ParseList<ast::Arg>& list, bool requireType) {
    WithLocation location(*this);

    auto lazy = parseLazy();
    auto returnRoot = parseReturnRoot();
    auto bind = parseBindType();

    auto name = tryMaybe(expect(Token::VarID, "expected parameter name"_v), return).id;
    ast::ParsePtr<ast::Type> type = nullptr;
    ast::ParsePtr<ast::Expr> def = nullptr;

    if(requireType) {
        expect(Token::opColon, "expected parameter type"_v);
        type = heap(ast::Type(parseType()));
    } else if(maybe(Token::opColon)) {
        type = heap(ast::Type(parseType()));
    }

    if(maybe(Token::opEquals)) {
        def = heap(ast::Expr(parseExpr()));
    }

    list.push(arena, ast::Arg {
        .source = context.addLocation(location),
        .name = name,
        .type = type,
        .def = def,
        .bind = bind,
        .returnRoot = returnRoot,
        .lazy = lazy,
    });
}

/*
 * A sub-pattern, with the binding convention a parameter and a `let` are written with: `Just(->v)`
 * takes the payload out and `Just(v)` refers to it where it lies.
 *
 * Read here rather than at the head of parsePattern, and that placement is the whole of the grammar
 * question. An alternative's arrow is the same token, so `| -> 1` has to keep reading as the missing
 * pattern it is; inside a constructor's parentheses or a record pattern's braces there is nothing
 * else a leading `->` could be.
 */
ast::Pat Parser::parseBoundPattern() {
    if(token.type != Token::opArrowR && token.type != Token::opAmp &&
       token.type != Token::opAmpArrowR) {
        return parsePattern();
    }

    // `&->` is one lexeme and sets both axes - see ast::Pat::bind. A bare `&` is read here as well
    // and refused in the resolver, where the reason for refusing it can be said.
    auto sink = token.type == Token::opAmpArrowR;
    auto bind = parseBindType();
    auto pat = parsePattern();
    pat.bind = bind;
    pat.sink = sink;
    return pat;
}

ast::FieldPat Parser::parseFieldPat() {
    WithLocation location(*this);

    auto qualified = maybe(Token::opTilde).isJust();
    if(qualified && token.type != Token::VarID) {
        error("expected variable name"_v);
    }

    if(auto data = maybe(Token::VarID)) {
        if(maybe(Token::opColon)) {
            return ast::FieldPat { data.unwrap().id, heap(ast::Pat(parseBoundPattern())) };
        } else {
            auto varPat = ast::Pat { .var = data.unwrap().id, .source = context.addLocation(location), .kind = ast::Pat::Var };
            return ast::FieldPat { qualified ? data.unwrap().id : StringId(), heap(ast::Pat(varPat)) };
        }
    } else {
        return ast::FieldPat { StringId(), heap(ast::Pat(parseBoundPattern())) };
    }
}

ast::Pat Parser::parsePattern() {
    WithLocation location(*this);

    // An operator section, which is the only pattern that starts with an operator: the matched
    // value is the left operand, so `>0` matches a value greater than zero. A lone `-` is not
    // one - it is the sign of a negative literal, which parseLeftPattern reads.
    if((token.type == Token::VarSym && !token.singleMinus) || token.type == Token::Grave) {
        auto op = parseQop();
        auto bound = parseLeftPattern();

        return ast::Pat {
            .section = { op.var, heap(bound) },
            .source = context.addLocation(location),
            .kind = ast::Pat::Section,
        };
    }

    auto allowRange = true;
    Maybe<ast::Pat> pat {};

    /*
     * The constructor the cursor is in - Implementation-Tooling.md §8.1's fifth kind.
     *
     * A `ConID` only. A lowercase name in pattern position is a *binding*, and the author is naming
     * something new rather than choosing among things that exist, so there is nothing to offer for
     * one - which is also why this is here rather than in parseLeftPattern, where the VarID is read.
     */
    if(atCursorName() && token.type == Token::ConID) {
        auto name = eatCursorName();
        return ast::Pat { .con = { name, nullptr }, .source = context.addLocation(location), .kind = ast::Pat::Con };
    }

    if(auto con = maybe(Token::ConID)) {
        if(token.type == Token::ParenL) {
            ast::ParseList<ast::FieldPat> fields;

            parens([&] {
                sepBy1([&] {
                    fields.push(arena, parseFieldPat());
                }, Token::Comma);
            });

            if(fields.size() == 1 && !fields.get(*arena, 0).field) {
                pat = Just(*(*arena)[fields.get(*arena, 0).pat]);
            } else {
                pat = Just(ast::Pat { .tup = fields, .source = context.addLocation(location), .kind = ast::Pat::Tup });
            }
        } else if(token.type == Token::BraceL) {
            auto p = parseLeftPattern();
            assertTrue(p.kind == ast::Pat::Tup);

            if(p.tup.size() == 1 && !p.tup.get(*arena, 0).field) {
                pat = Just(*(*arena)[p.tup.get(*arena, 0).pat]);
            } else {
                pat = Just(ast::Pat { .tup = p.tup, .source = context.addLocation(location), .kind = ast::Pat::Tup });
            }
        }

        allowRange = pat.isNothing();
        pat = Just(ast::Pat { .con = { con.unwrap().id, pat ? heap(pat.unwrap()) : nullptr }, .source = context.addLocation(location), .kind = ast::Pat::Con });
    } else {
        pat = Just(parseLeftPattern());
    }

    if(allowRange && (token.type == Token::opDotDot || token.type == Token::opDotDotEq)) {
        // The same two spellings a `for` header uses, and they mean the same thing here: `..`
        // excludes the upper bound and `..=` includes it. A pattern over a saturated type wants the
        // second - `0..=255` on a `U8` is the whole of it, and `0..256` is not writable at all.
        auto inclusive = token.type == Token::opDotDotEq;
        eat();

        auto to = parseLeftPattern();
        return ast::Pat { .range = { heap(pat.unwrap()), heap(to), inclusive }, .source = context.addLocation(location), .kind = ast::Pat::Range };
    }

    return pat.unwrap();
}

ast::Pat Parser::parseLeftPattern() {
    WithLocation location(*this);

    if(token.singleMinus) {
        eat();

        auto lit = (token.type == Token::Integer || token.type == Token::Float) ? toLiteral(location) : ({
            error("expected integer or float literal"_v);
            toLiteral({ .integer = 0 }, Token::Integer, location);
        });

        // The lexer only ever produces the positive magnitude, and the sign is recorded rather than
        // applied to it - see Pat::negative, and resolve/const.cpp for why the two have to stay
        // apart until something knows what type the number is of. `patternBound` applies it, at the
        // pivot's type.
        return ast::Pat {
            .lit = lit.lit,
            .negative = true,
            .source = lit.source,
            .kind = (ast::Pat::Kind)(ast::Pat::Lit + (lit.kind - ast::Expr::Lit)),
        };
    }

    if(token.type >= Token::FirstLiteral && token.type <= Token::LastLiteral) {
        auto lit = toLiteral(location);

        return ast::Pat {
            .lit = lit.lit,
            .source = lit.source,
            .kind = (ast::Pat::Kind)(ast::Pat::Lit + (lit.kind - ast::Expr::Lit)),
        };
    } else if(token.type == Token::kw_ || token.type == Token::kwElse) {
        eat();
        return ast::Pat { .source = context.addLocation(location), .kind = ast::Pat::Any };
    } else if(auto var = maybe(Token::VarID)) {
        if(maybe(Token::opAt)) {
            auto pat = parseLeftPattern();
            pat.asVar = var.unwrap().id;
            return pat;
        } else {
            return ast::Pat { .var = var.unwrap().id, .source = context.addLocation(location), .kind = ast::Pat::Var };
        }
    } else if(token.type == Token::ParenL) {
        Maybe<ast::Pat> pat;
        parens([&] { pat = Just(parsePattern()); });

        // A `(` left open at the end of a statement - see the same guard in parseSelExpr. The error
        // pattern is what M7 already made resolve to a wildcard that reports nothing further.
        if(!pat) return ast::Pat { .source = context.addLocation(location), .kind = ast::Pat::Error };

        return pat.unwrap();
    } else if(auto con = maybe(Token::ConID)) {
        // lpat can only contain a single constructor name.
        return { .con = { .name = con.unwrap().id, .pats = nullptr }, .source = context.addLocation(location), .kind = ast::Pat::Con };
    } else if(token.type == Token::BraceL) {
        ast::ParseList<ast::FieldPat> fields;
        braces([&] {
            sepBy1([&] {
                fields.push(arena, parseFieldPat());
            }, Token::Comma);
        });

        return { .tup = fields, .source = context.addLocation(location), .kind = ast::Pat::Tup };
    } else if(token.type == Token::BracketL) {
        ast::ParseList<ast::Pat> pats;
        brackets([&] {
            sepBy([&] {
                pats.push(arena, parsePattern());
            }, Token::Comma, Token::BracketR);
        });

        return { .arr = pats, .source = context.addLocation(location), .kind = ast::Pat::Arr };
    } else if(maybe(Token::opDotDot)) {
        auto name = expect(Token::VarID, "expected variable name"_v);
        return { .asVar = name ? name.unwrap().id : StringId(), .source = context.addLocation(location), .kind = ast::Pat::Rest };
    } else {
        error("expected pattern"_v);
        return { .source = context.addLocation(location), .kind = ast::Pat::Error };
    }
}

void Parser::parseAlt(ast::ParseList<ast::Alt>& list) {
    auto pat = parsePattern();
    auto expr = parseBlock(false);
    list.push(arena, { pat, expr });
}

ast::Expr Parser::parseQop() {
    WithLocation location(*this);

    if(auto t = maybe(Token::VarSym)) {
        return makeExpr(Var, var, t.unwrap().id, location);
    }

    expect(Token::Grave, "expected an operator"_v);
    auto op = expect(Token::VarID, "expected an operator"_v).from({ .id = StringId() }).id;
    expect(Token::Grave, "expected '`' after operator identifier"_v);

    return makeExpr(Var, var, op, location);
}

ast::FunKind Parser::parseFunKind() {
    if(maybe(Token::kwLens)) return ast::FunKind::Lens;
    if(maybe(Token::kwIter)) return ast::FunKind::Iter;

    return ast::FunKind::Plain;
}

void Parser::parseAttribute(ast::AttrList& list) {
    WithLocation location(*this);
    expect(Token::opAt, "expected '@'"_v);

    auto end = token.endColumn;
    auto name = tryMaybe(expectVarOrCon("expected identifier or type name"_v), return).id;
    ast::ParseList<ast::TupArg> args;

    // In this case we use significant whitespace. Since attributes can be added in many contexts,
    // we have to use whitespace to differentiate between an attribute argument list and other types of nodes.
    bool hasGap = token.startColumn != end;
    if(!hasGap && token.type == Token::ParenL) {
        parens([&] {
            return sepBy([&] {
                return parseTupArg(args);
            }, Token::Comma, Token::ParenR);
        });
    } else if(!hasGap && token.type == Token::BraceL) {
        WithLocation argsLocation(*this);
        auto expr = parseTupleExpr(argsLocation);
        args = ::move(expr.tup);
    }

    list.push(arena, ast::Attribute {
        .source = context.addLocation(location),
        .name = name,
        .args = args,
    });
}

void Parser::parseAttributes(ast::AttrList& list, bool isInline) {
    while(token.type == Token::opAt) {
        parseAttribute(list);
        if(!isInline) maybe(Token::EndOfStmt);
    }
}

ast::Constraint Parser::parseConstraint() {
    WithLocation location(*this);

    /*
     * `Num(a)`, `Num(Vec(I16, n))` - Implementation-Const-Generics.md §10.2.
     *
     * Not parseSimpleType, which is the *head* production: its list is the parameters a declaration
     * introduces, so bare identifiers are right there and an annotation is what §1.1 hangs off one.
     * A constraint's list is arguments, and each of them is exactly what an argument of a type
     * application is - so parseTypeApplicationArg reads them and no grammar is written for the
     * applied, literal-counted or array forms.
     */
    if(token.type == Token::ConID) {
        auto name = expect(Token::ConID, "expected type name"_v).from({ .id = StringId() }).id;
        ast::ParseList<ast::Type> args;

        if(token.type == Token::ParenL) {
            parens([&] {
                sepBy([&] {
                    args.push(arena, parseTypeApplicationArg());
                }, Token::Comma, Token::ParenR);
            });
        } else if(token.type == Token::VarID) {
            // The unparenthesized one-parameter form, `Show a`, which parseSimpleType also accepts.
            // A bare variable and nothing wider: an argument that needs a type constructor is one
            // written with the parentheses, and this way `Show a` cannot swallow the comma that
            // separates it from the next constraint in the list.
            WithLocation argLocation(*this);
            args.push(arena, parseAType(argLocation, nullptr));
        }

        return { .klass = { name, args }, .source = context.addLocation(location),
                 .kind = ast::Constraint::Class };
    }

    auto name = expect(Token::VarID, "expected type constraint"_v).from({ .id = StringId() }).id;

    if(maybe(Token::opDot)) {
        auto field = expect(Token::VarID, "expected constraint field name"_v).from({ .id = StringId() }).id;
        expect(Token::opColon, "expected ':' after field name"_v);
        auto type = parseType();

        return { .field = { name, field, heap(type) }, .source = context.addLocation(location), .kind = ast::Constraint::Field };
    }

    if(maybe(Token::opColon)) {
        WithLocation funLocation(*this);
        auto funKind = parseFunKind();

        /*
         * `n: Int` - a const parameter, Implementation-Const-Generics.md §1.2.
         *
         * Reached from the same colon the function constraint is reached from, and told apart by
         * what follows it: a `(` or a `lens`/`iter` marker is a callable's signature and anything
         * else is the type of a number. Parsed with parseAType for §1.4's reason - it stops before
         * `->`, which both keeps a class head's dependency arrow a separator and refuses
         * `n: (Int) -> Int`, a const parameter that would mean nothing.
         */
        if(funKind == ast::FunKind::Plain && token.type != Token::ParenL) {
            WithLocation constLocation(*this);
            auto type = parseAType(constLocation, nullptr);

            // `fn (n: Int = 0) vectorAt(...)` - the default, read exactly as a head parameter's is.
            // A context list is where a function declares its const parameters, so this is where a
            // function's default is written.
            auto def = parseGenDefault();

            return { .constant = { name, heap(type), def }, .source = context.addLocation(location),
                     .kind = ast::Constraint::Const };
        }

        expect(Token::ParenL, "expected function constraint"_v);
        ast::ParseList<ast::ArgDecl> args;

        sepBy([&] {
            parseTypeArg(args);
        }, Token::Comma, Token::ParenR);

        expectClose(Token::ParenR, "expected ')'"_v);
        expect(Token::opArrowR, "expected function return type"_v);

        auto retBind = parseResultBind();
        WithLocation retLocation(*this);
        auto ret = parseAType(retLocation, nullptr);
        auto type = makeType(Fun, fun, heap(ast::FunType { args, ret, funKind, retBind }), funLocation, nullptr);

        return { .fun = { name, heap(type) }, .source = context.addLocation(location), .kind = ast::Constraint::Function };
    }

    return { .source = context.addLocation(location), .kind = ast::Constraint::Any };
}

void Parser::parseConstraints(ast::ConstraintList& list) {
    parens([&] {
        sepBy([&] {
            list.push(arena, parseConstraint());
        }, Token::Comma, Token::ParenR);
    });
}

/*
 * `deriving (Eq, Ord, Logic)`, and `deriving Logic` for a list of one.
 *
 * The classes are named unapplied, since the type is the declaration this clause hangs off -
 * Analysis-Extensibility.md's primary spelling, chosen there for the reason field-level defaults
 * beat a `Default` class: what a type does is written where the type is.
 *
 * Both bracketings are read because the parenthesized list is the general form and a list of one is
 * most of what anybody writes. There is nothing to disambiguate - a `ConID` after `deriving` can
 * only be a class name - so admitting the bare form costs one branch and no grammar.
 *
 * Layout needs nothing said about it: `parseType` above has already stopped at the first token that
 * cannot continue a type, and a clause on a continuation line is still inside this declaration's
 * statement.
 */
void Parser::parseDeriving(ast::ParseList<ast::Derive>& list) {
    expect(Token::kwDeriving, "expected 'deriving'"_v);

    auto one = [&] {
        WithLocation location(*this);
        auto name = expect(Token::ConID, "expected a class name"_v).from({ .id = StringId() }).id;
        list.push(arena, ast::Derive { name, context.addLocation(location) });
    };

    if(token.type == Token::ParenL) {
        parens([&] {
            sepBy(one, Token::Comma, Token::ParenR);
        });
    } else {
        one();
    }
}

void Parser::parseArgDecl(ast::ParseList<ast::ArgDecl>& list) {
    auto lazy = parseLazy();
    auto returnRoot = parseReturnRoot();
    auto bind = parseBindType();

    if(token.type == Token::VarID) {
        // A leading identifier is only a parameter name if it is followed by ':'.
        // Otherwise, it is a bare generic type used as an unnamed argument (e.g. `(a, b) -> c`).
        SaveLexer save(lexer);
        auto savedToken = token;
        auto name = token.data.id;
        eat();

        if(maybe(Token::opColon)) {
            list.push(arena, ast::ArgDecl { parseType(), name, bind, returnRoot, lazy });
            return;
        }

        save.restore();
        token = savedToken;
    }

    list.push(arena, ast::ArgDecl { parseType(), StringId(), bind, returnRoot, lazy });
}

void Parser::parseTypeArg(ast::ParseList<ast::ArgDecl>& list) {
    parseArgDecl(list);
}

/*
 * One argument of a type application.
 *
 * A type, plus the one thing that is written where a type is and is not one: an integer literal, so
 * that `Vec(Float, 4)` parses. Confined to this position rather than accepted by `parseAType`,
 * because a number written anywhere *else* a type belongs is a mistake the parser can name on the
 * spot - `data BadField {3: Int}` reads better as "expected a type" than as a literal that survives
 * to the resolver - and only an application has a constructor that might have wanted a count.
 *
 * Whether the constructor in hand actually takes one is not decided here. `resolveType` rejects a
 * `Lit` everywhere except the vector constructors, which is where the message can say what the
 * number would have had to be.
 */
ast::Type Parser::parseTypeApplicationArg() {
    if(token.type != Token::Integer) return parseType();

    WithLocation location(*this);
    auto literal = heap(toLiteral(location));
    return makeType(Lit, lit, literal, location, nullptr);
}

/*
 * `= Int`, `= 4` - a generic parameter's default, or null where none was written.
 *
 * The two forms a written type *argument* takes, which is the point: what a default supplies is an
 * argument, so it is read by the same rules the argument it stands in for would have been. An
 * integer for a const parameter, an atomic type for a type parameter, and which of the two this
 * parameter admits is resolve's question rather than the grammar's - the same division
 * parseTypeApplicationArg already makes.
 *
 * `parseAType` and not `parseType`, for the reason the annotation beside it is: it stops before
 * `->`, so a class head's functional-dependency arrow stays a separator rather than being swallowed
 * as a function type's. `= (Int) -> Int` is thereby refused, which a default has no use for anyway.
 */
ast::ParsePtr<ast::Type> Parser::parseGenDefault() {
    if(!maybe(Token::opEquals)) return nullptr;

    WithLocation location(*this);

    if(token.type == Token::Integer) {
        auto literal = heap(toLiteral(location));
        return heap(makeType(Lit, lit, literal, location, nullptr));
    }

    return heap(parseAType(location, nullptr));
}

ast::Type Parser::parseType() {
    WithLocation location(*this);

    ast::AttrList attributeList;
    parseAttributes(attributeList, true);
    auto attributes = attributeList.isNotEmpty() ? heap(attributeList) : nullptr;

    auto funKind = parseFunKind();
    auto hasArgs = false;
    ast::ParseList<ast::ArgDecl> args;

    if(token.type == Token::ParenL || funKind != ast::FunKind::Plain) {
        hasArgs = true;

        parens([&] {
            sepBy([&] {
                parseTypeArg(args);
            }, Token::Comma, Token::ParenR);
        });
    }

    if(!hasArgs) {
        if(token.type == Token::ConID || token.type == Token::VarID) {
            auto isVar = token.type == Token::VarID;
            auto name = token.data.id;
            eat();

            auto base = isVar ? makeType(Gen, name, name, location, nullptr) : makeType(Con, name, name, location, nullptr);
            ast::ParseList<ast::Type> app;

            // In full types for cases where it is easily visible what's going on, we allow omitting parentheses.
            // This conveniently also prevents us from having to look too far ahead.
            if(token.type == Token::ParenL) {
                parens([&] {
                    sepBy1([&] {
                        app.push(arena, parseTypeApplicationArg());
                    }, Token::Comma);
                });
            } else if(token.type == Token::BraceL) {
                WithLocation appLocation(*this);
                app.push(arena, parseTupleType(appLocation, nullptr));
            } else if(token.type == Token::BracketL) {
                WithLocation appLocation(*this);
                app.push(arena, parseArrayType(appLocation, nullptr));
            } else if(token.type == Token::ConID || token.type == Token::VarID) {
                WithLocation appLocation(*this);

                auto isAppVar = token.type == Token::VarID;
                auto appName = token.data.id;
                eat();

                app.push(arena, isAppVar ? makeType(Gen, name, appName, appLocation, nullptr) : makeType(Con, name, appName, appLocation, nullptr));
            }

            if(app.isEmpty()) {
                base.attributes = attributes;
                return base;
            } else {
                return makeType(App, app, heap(ast::AppType { base, app }), location, attributes);
            }
        } else {
            return parseAType(location, attributes);
        }
    } else if(maybe(Token::opArrowR)) {
        WithLocation retLocation(*this);
        auto retBind = parseResultBind();
        return makeType(Fun, fun, heap(ast::FunType { .args = args, .ret = parseAType(retLocation, nullptr), .kind = funKind, .retBind = retBind }), location, attributes);
    } else {
        Maybe<ast::ArgDecl> arg = args.isNotEmpty() ? Just(args.get(*arena, 0)) : Nothing();
        if(arg && !arg.unwrap().name) {
            auto t = arg.unwrap().type;
            t.attributes = attributes;
            return t;
        } else {
            error("expected '->' after function type args"_v);
            return makeType(Error, name, StringId(), location, attributes);
        }
    }
}

ast::Type Parser::parseAType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes) {
    if(maybe(Token::VarSym, [&](Token& t) { return t.data.id == checkedRefId; })) {
        auto type = parseAType(location, nullptr);
        return makeType(Ref, to, heap(type), type.source, attributes);
    } else if(maybe(Token::VarSym, [&](Token& t) { return t.data.id == rawPtrId; })) {
        auto type = parseAType(location, nullptr);
        return makeType(Ptr, to, heap(type), type.source, attributes);
    } else if(maybe(Token::opAmp)) {
        auto type = parseAType(location, nullptr);
        return makeType(Borrow, to, heap(type), type.source, attributes);
    } else if(token.type == Token::ConID || token.type == Token::VarID) {
        auto isVar = token.type == Token::VarID;
        auto name = token.data.id;
        eat();

        auto base = isVar ? makeType(Gen, name, name, location, nullptr) : makeType(Con, name, name, location, nullptr);

        if(token.type == Token::ParenL) {
            ast::ParseList<ast::Type> args;

            parens([&] {
                sepBy1([&] {
                    args.push(arena, parseTypeApplicationArg());
                }, Token::Comma);
            });

            return makeType(App, app, heap(ast::AppType { base, args }), location, attributes);
        } else {
            base.attributes = attributes;
            return base;
        }
    } else if(token.type == Token::BraceL) {
        return parseTupleType(location, attributes);
    } else if(token.type == Token::BracketL) {
        return parseArrayType(location, attributes);
    } else if(maybe(Token::ParenL)) {
        auto t = parseType();
        expectClose(Token::ParenR, "expected ')'"_v);
        return t;
    } else {
        error("expected a type"_v);
        return makeType(Error, name, StringId(), location, attributes);
    }
}

ast::Type Parser::parseTupleType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes) {
    Maybe<ast::Type> type;

    braces([&] {
        ast::ParseList<ast::TupField> fields;

        sepBy([&] {
            if(auto n = maybeNode(Token::VarID)) {
                if(maybe(Token::opColon)) {
                    auto fieldType = parseType();

                    // `{read: Bool = False}` - what the field is when a construction leaves it
                    // out. Only a named field can have one, since an omitted positional field has
                    // no name to be omitted by.
                    ast::ParsePtr<ast::Expr> def = nullptr;
                    if(maybe(Token::opEquals)) def = heap(ast::Expr(parseExpr()));

                    fields.push(arena, ast::TupField { n.unwrap().payload.id, fieldType, def });
                } else {
                    auto gen = makeType(Gen, name, n.unwrap().payload.id, n.unwrap().node, nullptr);
                    fields.push(arena, ast::TupField { StringId(), gen, nullptr });
                }
            } else {
                fields.push(arena, ast::TupField { StringId(), parseType(), nullptr });
            }
        }, Token::Comma, Token::BraceR);

        if(fields.isEmpty()) {
            type = Just(makeType(Unit, name, StringId(), location, attributes));
        } else {
            type = Just(makeType(Tup, tup, { fields }, location, attributes));
        }
    });

    return type ? type.unwrap() : makeType(Error, name, StringId(), location, attributes);
}

ast::Type Parser::parseArrayType(const WithLocation& location, ast::ParsePtr<ast::AttrList> attributes) {
    Maybe<ast::Type> type;

    brackets([&] {
        auto from = parseType();
        if(maybe(Token::opColon)) {
            auto to = parseType();
            auto map = ast::Type::MapPayload { heap(from), heap(to) };
            type = Just(makeType(Map, map, map, location, attributes));
        } else if(maybe(Token::VarSym, [&](Token& t) { return t.data.id == arraySizeId; })) {
            auto size = parseExpr();
            auto arr = ast::Type::ArrPayload { heap(from), heap(size) };
            type = Just(makeType(Arr, arr, arr, location, attributes));
        } else {
            auto arr = ast::Type::ArrPayload { heap(from), nullptr };
            type = Just(makeType(Arr, arr, arr, location, attributes));
        }
    });

    return type ? type.unwrap() : makeType(Error, name, StringId(), location, attributes);
}

/*
 * A declaration head: the name, and the type variables it introduces.
 *
 * `allowDependency` is what separates a class head from a `data` or `alias` one. Only a class may
 * write `->` between its parameters, because what the arrow declares is a promise about the set of
 * instances - one `c` has one `a` - and neither of the other two has instances. Written anywhere
 * else the arrow is simply not a separator, so `data Pair(a -> b)` stops at the `a` and reports the
 * missing `)` rather than a rule nobody needs to know about.
 */
ast::SimpleType Parser::parseSimpleType(bool allowDependency) {
    auto name = expect(Token::ConID, "expected type name"_v).from({ .id = StringId() }).id;
    ast::ParseList<ast::GenParam> kind;
    U16 determined = 0;

    if(auto v = maybe(Token::VarID)) {
        // The unparenthesized one-parameter form, `data Maybe a`. No colon can follow it: the
        // annotation belongs inside the parentheses, and a head with a const parameter is a head
        // that has something to say - see §1.5 on why a head must write one.
        kind.push(arena, ast::GenParam { v.unwrap().id, nullptr });
    } else {
        maybeParens([&] {
            sepBy1([&] {
                WithLocation paramLocation(*this);
                auto n = expect(Token::VarID, "expected an identifier"_v).from({ .id = StringId() }).id;

                // `n: Int` - the const parameter of §1.1, and one `maybe` per parameter is the whole
                // of the change here. parseSimpleType had no other use for a colon in this list.
                ast::ParsePtr<ast::Type> annotation = nullptr;
                if(maybe(Token::opColon)) {
                    WithLocation constLocation(*this);
                    annotation = heap(parseAType(constLocation, nullptr));
                }

                /*
                 * `a = Int`, `n: Int = 4` - the default, and one `maybe` per parameter again.
                 *
                 * The `=` is unambiguous here even though `data` and `alias` both use one to open
                 * what follows the head, because a head that declares parameters at all declares
                 * them inside parentheses and this list ends at the `)`. The unparenthesized
                 * one-parameter form above takes no default for the same reason it takes no
                 * annotation: `data Maybe a = ...` would have two readings of its `=` and neither
                 * is the obvious one.
                 */
                auto def = parseGenDefault();

                if(n) kind.push(arena, ast::GenParam { n, annotation, def, context.addLocation(paramLocation) });
            }, [&] {
                if(allowDependency && token.type == Token::opArrowR) {
                    auto split = U16(kind.size());
                    eat();

                    // An arrow with nothing after it is one mistake, and both the list production
                    // ("expected an identifier") and the arity check in declareClass would report
                    // it as a second one. Ending the list here, with `determined` left as it was,
                    // is what keeps it to the message that names the rule.
                    if(token.type != Token::VarID) {
                        error("expected the parameters determined by the ones before the `->`"_v);
                        return false;
                    }

                    // One arrow, so the parameters split in two rather than into a graph. A class
                    // wanting `a -> b` and `b -> a` both is a declaration this cannot record, and
                    // saying so here is better than recording the first and silently losing the
                    // second.
                    if(determined) {
                        error("a class head may write only one `->`: the parameters before it determine every one after it"_v);
                    } else {
                        determined = split;
                    }

                    return true;
                }

                if(token.type != Token::Comma) return false;

                eat();
                return true;
            });
        });
    }

    return { name, kind, determined };
}

ast::Expr Parser::toLiteral(const Token::Payload& payload, Token::Type type, const WithLocation& source) {
    ast::Literal lit { .s = StringId() };
    U32 kind = 0;

    switch(type) {
        case Token::Integer:
            lit.i(payload.integer);
            kind = ast::Literal::Int;
            break;
        case Token::Float:
            lit.d(payload.floating);
            kind = ast::Literal::Double;
            break;
        case Token::Char:
            lit.c = payload.character;
            kind = ast::Literal::Char;
            break;
        case Token::String:
            lit.s = payload.id;
            kind = ast::Literal::String;
            break;
        default:
            assertTrue("Invalid literal type." == nullptr);
    }

    return makeExpr(Lit + kind, lit, lit, source);
}

ast::Expr Parser::toLiteral(const WithLocation& location) {
    // The location has to be resolved after the token is consumed: a location ends where the
    // current token starts, so building the literal first would give it a location that ends
    // before the literal itself begins.
    auto payload = token.data;
    auto type = token.type;
    eat();

    return toLiteral(payload, type, location);
}
