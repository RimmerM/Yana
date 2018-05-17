#include "parser.h"

using namespace ast;

inline Literal toLiteral(Token& tok) {
    Literal l;
    switch(tok.type) {
        case Token::Integer:
            l.i = tok.data.integer;
            l.type = Literal::Int;
            break;
        case Token::Float:
            l.f = tok.data.floating;
            l.type = Literal::Float;
            break;
        case Token::Char:
            l.c = tok.data.character;
            l.type = Literal::Char;
            break;
        case Token::String:
            l.s = tok.data.id;
            l.type = Literal::String;
            break;
        default: assert("Invalid literal type." == 0);
    }
    return l;
}

inline Literal toStringLiteral(Id name) {
    Literal l;
    l.s = name;
    l.type = Literal::String;
    return l;
}

static const char kRefSigil = '&';
static const char kValSigil = '*';
static const char kPtrSigil = '%';

Parser::Parser(Context& context, ast::Module& module, const char* text):
    text(text),
    context(context),
    diag(context.diagnostics),
    module(module),
    buffer(module.buffer),
    lexer(context, diag, text, &token) {

    qualifiedId = Context::nameHash("qualified", 9);
    hidingId = Context::nameHash("hiding", 6);
    fromId = Context::nameHash("from", 4);
    asId = Context::nameHash("as", 2);
    refId = Context::nameHash(&kRefSigil, 1);
    ptrId = Context::nameHash(&kPtrSigil, 1);
    valId = Context::nameHash(&kValSigil, 1);
    downtoId = Context::nameHash("downto", 6);
    hashId = Context::nameHash("#", 1);
    minusId = Context::nameHash("-", 1);
    stepId = Context::nameHash("step", 4);

    lexer.next();
}

void Parser::parseModule() {
    auto errorCount = context.diagnostics.errorCount();
    auto warningCount = context.diagnostics.warningCount();

    withLevel([=] {
        while(true) {
            if(token.type == Token::EndOfFile) {
                break;
            }

            while(token.type == Token::EndOfStmt) {
                eat();
            }

            if(token.type == Token::kwImport) {
                parseImport();
            } else if(token.type == Token::kwInfixL || token.type == Token::kwInfixR) {
                parseFixity();
            } else {
                auto nextDecl = [&](List<Attribute>* attributes) {
                    bool exported = false;
                    if(token.type == Token::kwPub) {
                        eat();
                        exported = true;
                    }

                    auto decl = parseDecl();
                    decl->attributes = attributes;
                    decl->exported = exported;

                    module.decls << decl;
                };

                auto attributes = parseAttributes();
                if(attributes != nullptr && token.type == Token::opColon) {
                    eat();
                    withLevel([&] {
                        sepBy([&] {
                            auto localAttributes = parseAttributes();
                            if(localAttributes) {
                                auto end = localAttributes;
                                while(end->next) end = end->next;

                                end->next = attributes;
                            } else {
                                localAttributes = attributes;
                            }

                            nextDecl(localAttributes);
                            return true;
                        }, Token::EndOfStmt, Token::EndOfBlock);
                        return true;
                    });
                } else {
                    nextDecl(attributes);
                }
            }

            if(token.type == Token::EndOfStmt) {
                continue;
            } else if(token.type == Token::EndOfBlock && token.startColumn == 0) {
                break;
            } else {
                // The previous declaration did not parse all tokens.
                // Skip ahead until we are at the root level again, then continue parsing.
                error("expected declaration end"_buffer);
                while(token.startColumn > 0) {
                    eat();
                }

                if(token.type == Token::EndOfBlock) {
                    break;
                }
            }
        }

        // Dummy return value to withLevel.
        return true;
    });

    module.errorCount = context.diagnostics.errorCount() - errorCount;
    module.warningCount = context.diagnostics.warningCount() - warningCount;
}

void Parser::parseImport() {
    assert(token.type == Token::kwImport);
    eat();

    bool qualified = false;
    if(token.type == Token::VarID && token.data.id == qualifiedId) {
        eat();
        qualified = true;
    }

    Id name;
    if(token.type == Token::VarID || token.type == Token::ConID) {
        name = token.data.id;
        eat();
    } else {
        error("expected symbol name"_buffer);
        name = 0;
    }

    List<Id>* include = maybeParens([=] {
        return sepBy([=] {
            Id included;
            if(token.type == Token::VarID || token.type == Token::ConID) {
                included = token.data.id;
                eat();
            } else {
                error("expected symbol name"_buffer);
                included = 0;
            }
            return included;
        }, Token::Comma, Token::ParenR);
    });

    List<Id>* exclude = nullptr;
    if(token.type == Token::VarID && token.data.id == hidingId) {
        eat();
        exclude = parens([=] {
            return sepBy([=] {
                Id hiddenName;
                if(token.type == Token::VarID || token.type == Token::ConID) {
                    hiddenName = token.data.id;
                    eat();
                } else {
                    error("expected symbol name"_buffer);
                    hiddenName = 0;
                }
                return hiddenName;
            }, Token::Comma, Token::ParenR);
        });
    }

    Id asName = 0;
    if(token.type == Token::VarID && token.data.id == asId) {
        eat();

        if(token.type == Token::ConID) {
            asName = token.data.id;
            eat();
        } else {
            error("expected identifier"_buffer);
        }
    }

    auto import = module.imports.push();
    import->localName = asName ? asName : name;
    import->from = name;
    import->qualified = qualified;
    import->include = include;
    import->exclude = exclude;
}

void Parser::parseFixity() {
    auto fixity = node([=]() -> Fixity* {
        Fixity::Kind kind;
        if(token.type == Token::kwInfixR) {
            kind = Fixity::Right;
        } else {
            kind = Fixity::Left;
        }
        eat();

        U32 precedence;
        if(token.type == Token::Integer) {
            precedence = (U32)token.data.integer;
            eat();
        } else {
            error("expected operator precedence"_buffer);
            precedence = 9;
        }

        auto op = parseQop();
        return new (buffer) Fixity{op->name, precedence, kind};
    });

    module.ops << *fixity;
}

Decl* Parser::parseDecl() {
    if(token.type == Token::kwAlias) {
        return parseTypeDecl();
    } else if(token.type == Token::kwData) {
        return parseDataDecl();
    } else if(token.type == Token::kwForeign) {
        return parseForeignDecl();
    } else if(token.type == Token::kwFn) {
        return parseFunDecl(true);
    } else if(token.type == Token::kwClass) {
        return parseClassDecl();
    } else if(token.type == Token::kwInstance) {
        return parseInstanceDecl();
    } else if(token.type == Token::kwAtData) {
        return parseAttrDecl();
    } else {
        auto expr = parseExpr();
        if(expr->type == Expr::Decl) {
            ((DeclExpr*)expr)->isGlobal = true;
        } else if(expr->type == Expr::Ret) {
            error("return statements cannot be used in a global scope"_buffer, expr);
        }
        return new (buffer) StmtDecl(expr);
    }
}

Decl* Parser::parseFunDecl(bool requireBody) {
    return node([=]() -> Decl* {
        if(token.type == Token::kwFn) {
            eat();
        } else {
            error("expected function declaration"_buffer);
        }

        Id name;
        if(token.type == Token::VarID) {
            name = token.data.id;
            eat();
        } else {
            name = 0;
            error("expected function name"_buffer);
        }

        auto args = parens([=] {
            return sepBy([=] {
                return parseArg(true);
            }, Token::Comma, Token::ParenR);
        });

        Type *ret = nullptr;
        if(token.type == Token::opArrowR) {
            eat();
            ret = parseType();
        }

        bool implicitReturn;
        Expr *body;
        if(token.type == Token::opEquals) {
            implicitReturn = true;
            body = node([=]() -> Expr* {
                eat();
                auto expr = parseExpr();
                if(token.type == Token::kwWhere) {
                    auto l = token.endLine;
                    eat();
                    auto decls = list(parseVarDecl(l));
                    decls->next = list(expr);
                    return new(buffer) MultiExpr(decls);
                } else {
                    return expr;
                }
            });
        } else if(token.type == Token::opBar) {
            implicitReturn = true;
            auto cases = withLevel([=] {
                return sepBy1([=] {
                    if(token.type == Token::opBar) {
                        eat();
                    } else {
                        error("expected '|'"_buffer);
                    }
                    return parseAlt();
                }, Token::EndOfStmt);
            });

            Expr* pivot = nullptr;
            if(args && args->next) {
                auto arg = args;
                auto tup = list(TupArg(0, new (buffer) VarExpr(arg->item.name)));
                auto t = tup;

                arg = arg->next;
                while(arg) {
                    t->next = list(TupArg(0, new (buffer) VarExpr(arg->item.name)));
                    t = t->next;
                    arg = arg->next;
                }

                pivot = new (buffer) TupExpr(tup);
            } else if(args) {
                pivot = new (buffer) VarExpr(args->item.name);
            }

            body = new (buffer) CaseExpr(pivot, cases);
        } else if(token.type == Token::opColon) {
            implicitReturn = false;
            body = parseBlock(true);
        } else {
            body = nullptr;
            implicitReturn = false;

            if(requireBody) {
                error("expected function body"_buffer);
            }
        }

        return new(buffer) FunDecl(name, body, args, ret, implicitReturn);
    });
}

Decl* Parser::parseDataDecl() {
    return node([=]() -> DataDecl* {
        assert(token.type == Token::kwData);
        eat();

        bool qualified = false;
        if(token.type == Token::VarID && token.data.id == qualifiedId) {
            qualified = true;
            eat();
        }

        auto type = parseSimpleType();

        List<Con>* cons;
        if(token.type == Token::opEquals) {
            eat();

            cons = sepBy1([=] {
                return node([=] {return parseCon();});
            }, Token::opBar);
        } else if(token.type == Token::BraceL) {
            cons = list(Con{type->name, parseTupleType()});
        } else {
            error("expected '=' or '{' after type name"_buffer);
            cons = nullptr;
        }

        return new (buffer) DataDecl(type, cons, qualified);
    });
}

Decl* Parser::parseTypeDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwAlias);
        eat();

        auto name = parseSimpleType();
        if(token.type == Token::opEquals) {
            eat();
        } else {
            error("expected '='"_buffer);
        }

        auto type = parseType();
        return new (buffer) AliasDecl(name, type);
    });
}

Decl* Parser::parseForeignDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwForeign);
        eat();

        bool isFun = false;
        if(token.type == Token::kwFn) {
            eat();
            isFun = true;
        }

        bool stringName = token.type == Token::String;
        Id name = 0;
        if(token.type == Token::VarID || token.type == Token::String) {
            name = token.data.id;
            eat();
        } else {
            error("expected identifier"_buffer);
        }

        Id importName = 0;
        if(token.type == Token::VarID && token.data.id == asId) {
            eat();
            if(token.type == Token::VarID) {
                importName = token.data.id;
                eat();
            } else {
                error("expected an identifier"_buffer);
            }
        } else if(stringName) {
            error("expected 'as' and foreign import name"_buffer);
        }

        // A normal function type looks exactly like a function declaration when directly after the name.
        if(token.type == Token::opColon) {
            eat();
        } else if(!isFun) {
            error("expected ':'"_buffer);
        }

        auto type = parseType();

        Id from = 0;
        if(token.type == Token::VarID && token.data.id == fromId) {
            eat();
            if(token.type == Token::String) {
                from = token.data.id;
                eat();
            } else {
                error("expected a string"_buffer);
            }
        }

        return new(buffer) ForeignDecl(name, importName, from, type);
    });
}

Decl* Parser::parseClassDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwClass);
        eat();

        auto type = parseSimpleType();
        if(token.type == Token::opColon) {
            eat();
        } else {
            error("expected ':' after class declaration"_buffer);
        }

        auto decls = withLevel([=] {
            return sepBy([=] {
                return (FunDecl*)parseFunDecl(false);
            }, Token::EndOfStmt, Token::EndOfBlock);
        });

        return new(buffer) ClassDecl(type, decls);
    });
}

Decl* Parser::parseInstanceDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwInstance);
        eat();

        auto type = parseType();
        if(token.type == Token::opColon) {
            eat();
        } else {
            error("expected ':' after instance declaration"_buffer);
        }

        auto decls = withLevel([=] {
            return sepBy([=] {
                return parseDecl();
            }, Token::EndOfStmt, Token::EndOfBlock);
        });

        return new(buffer) InstanceDecl(type, decls);
    });
}

ast::Decl* Parser::parseAttrDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwAtData);
        eat();

        Id name = 0;
        if(token.type == Token::VarID || token.type == Token::ConID) {
            name = token.data.id;
            eat();
        } else {
            error("expected identifier"_buffer);
        }

        if(token.type == Token::ParenL || token.type == Token::BraceL || token.type == Token::BracketL) {
            return new (buffer) AttrDecl(name, parseType());
        } else {
            error("expected attribute type"_buffer);
            return new (buffer) AttrDecl(name, new (buffer) Type(Type::Error));
        }
    });
}

Expr* Parser::parseBlock(bool isFun) {
    // To make the code more readable we avoid using '=' inside expressions, and use '->' instead.
    if(token.type == (isFun ? Token::opEquals : Token::opArrowR)) {
        eat();
        return parseExpr();
    } else {
        if(token.type == Token::opColon) {
            eat();
        } else {
            error("expected ':'"_buffer);
        }

        return withLevel([=] {
            return parseExprSeq();
        });
    }
}

Expr* Parser::parseExprSeq() {
    /*
     * exprseq	→	expr
     * 			|	expr0; …; exprn	(statements, n ≥ 2)
     */
    auto list = sepBy1([=] {return parseTypedExpr();}, Token::EndOfStmt);
    if(list->next) {
        return new(buffer) MultiExpr(list);
    } else {
        return list->item;
    }
}

Expr* Parser::parseExpr() {
    return parseTypedExpr();
}

Expr* Parser::parseTypedExpr() {
    /*
     * typedexpr	→	infixexpr :: type
     *				|	infixexpr
     */

    auto expr = parseInfixExpr();
    if(token.type == Token::opColonColon) {
        eat();
        auto type = parseType();
        return new(buffer) CoerceExpr(expr, type);
    } else {
        return expr;
    }
}

Expr* Parser::parseInfixExpr() {
    /*
     * infixexp		→	pexp qop infixexp			(infix operator application)
     * 				|	pexp = infixexp				(assignment)
     *				|	pexp
     */
    auto lhs = parsePrefixExpr();
    if(token.type == Token::opEquals) {
        eat();
        auto rhs = parseExpr();
        if(!rhs) error("Expected an expression after assignment."_buffer);

        return new(buffer) AssignExpr(lhs, rhs);
    } else if(token.type == Token::VarSym || token.type == Token::Grave) {
        // Binary operator.
        auto op = parseQop();
        auto rhs = parseInfixExpr();
        if(!rhs) error("Expected a right-hand side for a binary operator."_buffer);

        return new(buffer) InfixExpr(op, lhs, rhs);
    } else {
        // Single expression.
        return lhs;
    }
}

Expr* Parser::parsePrefixExpr() {
    /*
     * pexp		→	varsym lexp				(prefix operator application)
     *			|	lexp
     */
    if(token.type == Token::VarSym) {
        auto op = node([=]() -> VarExpr* {
            auto id = token.data.id;
            eat();
            return new (buffer) VarExpr(id);
        });

        auto expr = parsePrefixExpr();
        if(expr->type == Expr::Lit && op->name == minusId) {
            auto lit = &((LitExpr*)expr)->literal;
            if(lit->type == Literal::Int) {
                lit->i = -lit->i;
                return expr;
            } else if(lit->type == Literal::Float) {
                lit->f = -lit->f;
                return expr;
            }
        }

        return new (buffer) PrefixExpr(op, expr);
    } else {
        return parseLeftExpr();
    }
}

Expr* Parser::parseLeftExpr() {
    return node([=]() -> Expr* {
        if(token.type == Token::kwLet) {
            auto l = token.endLine;
            eat();
            return parseVarDecl(l);
        } else if(token.type == Token::kwMatch) {
            return parseCaseExpr();
        } else if(token.type == Token::kwIf) {
            return parseIfExpr();
        } else if(token.type == Token::kwWhile) {
            eat();
            auto cond = parseExpr();
            auto loop = parseBlock(false);
            return new (buffer) WhileExpr(cond, loop);
        } else if(token.type == Token::kwFor) {
            eat();
            Id var;
            if(token.type == Token::VarID) {
                var = token.data.id;
                eat();
            } else {
                error("expected for loop variable"_buffer);
                var = 0;
            }

            if(token.type == Token::kwIn) {
                eat();
            } else {
                error("expected 'in'"_buffer);
            }

            auto from = parseSelExpr();

            bool reverse = false;
            if(token.type == Token::opDotDot) {
                eat();
            } else if(token.type == Token::VarID && token.data.id == downtoId) {
                eat();
                reverse = true;
            } else {
                error("expected '..'"_buffer);
            }

            auto to = parseSelExpr();

            Expr* step;
            if(token.type == Token::VarID && token.data.id == stepId) {
                eat();
                step = parseSelExpr();
            } else {
                step = nullptr;
            }

            auto body = parseBlock(false);
            return new (buffer) ForExpr(var, from, to, body, step, reverse);
        } else if(token.type == Token::kwReturn) {
            eat();
            auto body = parseExpr();
            return new (buffer) RetExpr(body);
        } else {
            return parseAppExpr();
        }
    });
}

Expr* Parser::parseAppExpr() {
    return node([=] {
        auto base = parseBaseExpr();
        return parseChain(base);
    });
}

Expr* Parser::parseChain(Expr *base) {
    return node([=] {
        if(token.type == Token::ParenL) {
            auto args = parens([=] {
                return sepBy([=] {
                    return parseTupArg();
                }, Token::Comma, Token::ParenR);
            });

            return parseChain(new (buffer) AppExpr(base, args));
        } else if(token.type == Token::opDot) {
            eat();
            auto app = parseSelExpr();
            return parseChain(new (buffer) FieldExpr(base, app));
        } else {
            return base;
        }
    });
}

Expr* Parser::parseCaseExpr() {
    assert(token.type == Token::kwMatch);
    eat();

    auto pivot = parseExpr();

    List<Alt>* alts = nullptr;
    if(token.type == Token::opColon) {
        eat();
        alts = withLevel([=] {
            return sepBy1([=] {
                return parseAlt();
            }, Token::EndOfStmt);
        });
    } else {
        error("Expected ':' after match-expression."_buffer);
    }

    return new(buffer) CaseExpr(pivot, alts);
}

Expr* Parser::parseIfExpr() {
    assert(token.type == Token::kwIf);
    eat();

    if(token.type == Token::opColon) {
        eat();

        // Multi-way if.
        auto list = withLevel([=] {
            return sepBy1([=]() -> IfCase {
                Expr *cond;
                if(token.type == Token::kw_ || token.type == Token::kwElse) {
                    eat();

                    Literal lit;
                    lit.type = Literal::Bool;
                    lit.b = true;

                    cond = new(buffer) LitExpr(lit);
                } else {
                    cond = parseExpr();
                }

                if(token.type == Token::opArrowR) eat();
                else error("expected '->' after if condition"_buffer);

                auto then = parseExpr();
                return IfCase(cond, then);
            }, Token::EndOfStmt);
        });
        return new(buffer) MultiIfExpr(list);
    } else {
        auto cond = parseExpr();

        Expr* expr;
        if(token.type == Token::opColon) {
            expr = parseBlock(false);
        } else {
            if(token.type == Token::kwThen) {
                eat();
            } else {
                error("Expected 'then' after if-expression."_buffer);
            }

            expr = parseExpr();
        }

        // We should only eat the statement end if there is an else after.
        SaveLexer save(lexer);
        auto savedToken = token;

        if(token.type == Token::EndOfStmt) eat();
        if(token.type == Token::kwElse) {
            eat();

            Expr* otherwise;
            if(token.type == Token::opColon) {
                otherwise = parseBlock(false);
            } else {
                otherwise = parseExpr();
            }

            return new (buffer) IfExpr(cond, expr, otherwise);
        } else {
            save.restore();
            token = savedToken;
            return new (buffer) IfExpr(cond, expr, nullptr);
        }
    }
}

Expr* Parser::parseBaseExpr() {
    if(token.type == Token::ParenL) {
        return node([=]() -> Expr* {
            eat();

            // Cases to handle:
            // () block
            // (expr)
            // (varexpr) block
            // (varexpr: type) block
            // (varexpr, ...) block
            // (varexpr: type, ...) block
            if(token.type == Token::ParenR) {
                // () block
                eat();
                return new (buffer) FunExpr(nullptr, parseBlock(false));
            } else {
                auto e = parseExpr();
                if(token.type == Token::ParenR) {
                    // Cases to handle:
                    // (expr)
                    // (varexpr) block
                    eat();
                    if(e->type == Expr::Var && (token.type == Token::opColon || token.type == Token::opArrowR)) {
                        auto arg = list(Arg{((VarExpr*)e)->name, nullptr, nullptr});
                        return new (buffer) FunExpr(arg, parseBlock(false));
                    } else {
                        return new(buffer) NestedExpr(e);
                    }
                } else {
                    // Cases to handle:
                    // (varexpr: type) block
                    // (varexpr: type, ...) block
                    // (varexpr, ...) block
                    Id firstName = 0;
                    if(e->type == Expr::Var) {
                        firstName = ((VarExpr*)e)->name;
                    } else {
                        error("expected argument name"_buffer);
                    }

                    Type* firstType = nullptr;
                    if(token.type == Token::opColon) {
                        eat();
                        firstType = parseType();
                    }

                    auto args = list(Arg{firstName, firstType, nullptr});
                    if(token.type == Token::Comma) {
                        eat();
                        args->next = sepBy1([=] {
                            return parseArg(false);
                        }, Token::Comma);
                    }

                    if(token.type == Token::ParenR) {
                        eat();
                    } else {
                        error("expected ',' or ')' in argument list"_buffer);
                    }

                    return new (buffer) FunExpr(args, parseBlock(false));
                }
            }
        });
    } else if(token.type == Token::BraceL) {
        return parseTupleExpr();
    } else if(token.type == Token::BracketL) {
        return parseArrayExpr();
    } else if(token.type == Token::ConID) {
        return node([=]() -> Expr* {
            auto type = node([=]() -> ConType* {
                auto name = token.data.id;
                eat();
                return new (buffer) ConType(name);
            });

            if(token.type == Token::ParenL) {
                auto args = parens([=] {
                    return sepBy([=] {
                        return parseTupArg();
                    }, Token::Comma, Token::ParenR);
                });

                return new (buffer) ConExpr(type, args);
            } else if(token.type == Token::BraceL) {
                auto expr = (TupExpr*)parseTupleExpr();
                return new (buffer) ConExpr(type, expr->args);
            } else {
                return new (buffer) ConExpr(type, nullptr);
            }
        });
    } else {
        return parseSelExpr();
    }
}

Expr* Parser::parseSelExpr() {
    return node([=]() -> Expr* {
        if(token.type >= Token::FirstLiteral && token.type <= Token::LastLiteral) {
            if(token.type == Token::String) {
                return parseStringExpr();
            } else {
                auto expr = new(buffer) LitExpr(toLiteral(token));
                eat();
                return expr;
            }
        } else if(token.type == Token::VarID) {
            auto id = token.data.id;
            eat();
            return new (buffer) VarExpr(id);
        } else if(token.type == Token::ParenL) {
            return new (buffer) NestedExpr(parens([=] {return parseExpr();}));
        } else {
            error("expected an expression"_buffer);
            return new (buffer) Expr(Expr::Error);
        }
    });
}

Expr* Parser::parseStringExpr() {
    assert(token.type == Token::String);
    auto string = token.data.id;
    eat();

    // Check if the string contains formatting.
    if(token.type == Token::StartOfFormat) {
        // Parse one or more formatting expressions.
        // The first one consists of just the first string chunk.
        auto l = list(FormatChunk{string, nullptr});
        auto p = l;

        while(token.type == Token::StartOfFormat) {
            eat();
            auto expr = parseExpr();

            if(token.type == Token::EndOfFormat) {
                eat();
            } else {
                error("expected end of string format after this expression."_buffer);
            }

            assertTrue(token.type == Token::String);
            auto endString = token.data.id;

            eat();
            p->next = list(FormatChunk{endString, expr});
            p = p->next;
        }

        return new(buffer) FormatExpr(l);
    } else {
        return new(buffer) LitExpr(toStringLiteral(string));
    }
}

TupArg Parser::parseTupArg() {
    bool qualified = false;
    if(token.type == Token::VarSym && token.data.id == hashId) {
        eat();
        qualified = true;
    }

    auto arg = parseExpr();
    if(arg->type != Expr::Var && qualified) {
        error("expected variable name"_buffer);
    }

    if(arg->type == Expr::Assign && ((AssignExpr*)arg)->target->type == Expr::Var) {
        auto assign = (AssignExpr*)arg;
        auto var = (VarExpr*)assign->target;
        auto name = var->name;
        return TupArg{name, assign->value};
    } else if(qualified && arg->type == Expr::Var) {
        return TupArg{((VarExpr*)arg)->name, arg};
    } else {
        return TupArg{0, arg};
    }
}

Arg Parser::parseArg(bool requireType) {
    DeclExpr::Mutability m;
    Id name = 0;

    if(token.type == Token::VarSym && token.data.id == refId) {
        eat();
        m = DeclExpr::Ref;
    } else if(token.type == Token::VarSym && token.data.id == valId) {
        eat();
        m = DeclExpr::Val;
    } else {
        m = DeclExpr::Immutable;
    }

    if(token.type == Token::VarID) {
        name = token.data.id;
        eat();
    } else {
        error("expected parameter name"_buffer);
    }

    Type* type = nullptr;
    if(token.type == Token::opColon) {
        eat();
        type = parseType();

        if(m == DeclExpr::Ref) {
            type = new (buffer) RefType(type);
        } else if(m == DeclExpr::Val) {
            type = new (buffer) ValType(type);
        }
    } else if(requireType) {
        error("expected parameter type"_buffer);
    }

    Expr* def = nullptr;
    if(token.type == Token::opEquals) {
        eat();
        def = parseExpr();
    }

    return Arg{name, type, def};
}

ArgDecl Parser::parseTypeArg() {
    return parseArgDecl();
}

ArgDecl Parser::parseArgDecl() {
    if(token.type == Token::VarID) {
        auto gen = node([=]() -> GenType* {
            if(token.type != Token::VarID) {
                error("expected identifier"_buffer);
            }

            auto id = token.data.id;
            eat();
            return new (buffer) GenType(id);
        });

        if(token.type == Token::opColon) {
            eat();
            return ArgDecl(parseType(), gen->con);
        } else {
            return ArgDecl(gen, 0);
        }
    } else {
        return ArgDecl(parseType(), 0);
    }
}

Expr* Parser::parseVarDecl(U32 line) {
    if(token.startLine == line) {
        return parseDeclExpr();
    }

    // Parse one or more declarations, separated as statements.
    auto list = withLevel([=] {
        return sepBy1([=] {
            return parseDeclExpr();
        }, Token::EndOfStmt);
    });

    if(list->next) {
        return new(buffer) MultiExpr(list);
    } else {
        return list->item;
    }
}

Expr* Parser::parseDeclExpr() {
    DeclExpr::Mutability m;
    if(token.type == Token::VarSym && token.data.id == refId) {
        eat();
        m = DeclExpr::Ref;
    } else if(token.type == Token::VarSym && token.data.id == valId) {
        eat();
        m = DeclExpr::Val;
    } else {
        m = DeclExpr::Immutable;
    }

    auto pat = parsePattern();

    if(token.type == Token::opEquals) {
        eat();
        auto expr = parseExpr();

        Expr* otherwise = nullptr;
        if(token.type == Token::kwElse) {
            eat();
            otherwise = parseExpr();
        }

        Expr* in = nullptr;
        if(token.type == Token::kwIn) {
            eat();
            in = parseExpr();
        }

        return new(buffer) DeclExpr(pat, expr, in, otherwise, m);
    } else {
        return new(buffer) DeclExpr(pat, nullptr, nullptr, nullptr, m);
    }
}

Alt Parser::parseAlt() {
    /*
     * alt	→	pat -> exp [where decls]
     * 		|	pat gdpat [where decls]
     * 		|		    					(empty alternative)
     */
    auto pat = parsePattern();
    auto exp = parseBlock(false);
    return Alt{pat, exp};
}

VarExpr* Parser::parseVar() {
    /*
     * var	→	varid | ( varsym )
     */
    if(token.type == Token::VarID) {
        auto id = token.data.id;
        eat();
        return new (buffer) VarExpr(id);
    } else if(token.type == Token::ParenL) {
        eat();
        if(token.type == Token::VarSym) {
            auto id = token.data.id;
            eat();
            if(token.type == Token::ParenR) {
                eat();
            } else {
                error("expected ')'"_buffer);
            }

            return new (buffer) VarExpr(id);
        } else {
            error("expected symbol"_buffer);
            return new (buffer) VarExpr(0);
        }
    } else {
        error("expected identifier"_buffer);
        return new (buffer) VarExpr(0);
    }
}

VarExpr* Parser::parseQop() {
    /*
     * qop	→	qvarsym | `qvarid`
     */
    return node([=]() -> VarExpr* {
        if(token.type == Token::VarSym) {
            auto id = token.data.id;
            eat();
            return new (buffer) VarExpr(id);
        } else if(token.type == Token::Grave) {
            eat();
            if(token.type == Token::VarID) {
                auto id = token.data.id;
                eat();

                if(token.type == Token::Grave) {
                    eat();
                } else {
                    error("Expected '`' after operator identifier"_buffer);
                }

                return new (buffer) VarExpr(id);
            }
        }

        error("expected an operator"_buffer);
        return new (buffer) VarExpr(0);
    });
}

Type* Parser::parseType() {
    return node([=]() -> Type* {
        bool hasArgs = false;
        List<ArgDecl>* args = maybeParens([&] {
            hasArgs = true;
            return sepBy([=] {
                return parseTypeArg();
            }, Token::Comma, Token::ParenR);
        });

        if(!hasArgs) {
            if(token.type == Token::ConID || token.type == Token::VarID) {
                auto base = node([=]() -> Type* {
                    auto isVar = token.type == Token::VarID;
                    auto id = token.data.id;
                    eat();
                    return isVar ? (Type*)(new (buffer) GenType(id)) : (Type*)(new(buffer) ConType(id));
                });

                // In full types for cases where it is easily visible what's going on, we allow omitting parentheses.
                // This conveniently also prevents us from having to look too far ahead.
                if(token.type == Token::ParenL) {
                    auto type = parens([=] {
                        return sepBy1([=] {
                            return parseType();
                        }, Token::Comma);
                    });
                    return new (buffer) AppType(base, type);
                } else if(token.type == Token::BraceL) {
                    return new (buffer) AppType(base, list(parseTupleType()));
                } else if(token.type == Token::BracketL) {
                    return new (buffer) AppType(base, list(parseArrayType()));
                } else if(token.type == Token::ConID) {
                    auto con = node([=]() -> Type* {
                        auto id = token.data.id;
                        eat();
                        return new(buffer) ConType(id);
                    });
                    return new (buffer) AppType(base, list(con));
                } else if(token.type == Token::VarID) {
                    auto con = node([=]() -> Type* {
                        auto id = token.data.id;
                        eat();
                        return new(buffer) GenType(id);
                    });
                    return new (buffer) AppType(base, list(con));
                } else {
                    return base;
                }
            } else {
                return parseAType();
            }
        } else if(token.type == Token::opArrowR) {
            eat();
            return new (buffer) FunType(args, parseAType());
        } else {
            auto arg = args->next ? nullptr : &args->item;
            if(arg && !arg->name) {
                return arg->type;
            } else {
                error("expected '->' after function type args"_buffer);
                return new (buffer) Type(Type::Error);
            }
        }
    });
}

Type* Parser::parseAType() {
    if(token.type == Token::VarSym && token.data.id == ptrId) {
        eat();
        auto type = parseAType();
        return new (buffer) PtrType(type);
    } else if(token.type == Token::VarSym && token.data.id == refId) {
        eat();
        auto type = parseAType();
        return new (buffer) RefType(type);
    } else if(token.type == Token::VarSym && token.data.id == valId) {
        eat();
        auto type = parseAType();
        return new (buffer) ValType(type);
    } else if(token.type == Token::ConID || token.type == Token::VarID) {
        auto base = node([=]() -> Type* {
            auto isVar = token.type == Token::VarID;
            auto id = token.data.id;
            eat();
            return isVar ? (Type*)(new (buffer) GenType(id)) : (Type*)(new(buffer) ConType(id));
        });

        if(token.type == Token::ParenL) {
            auto type = parens([=] {
                return sepBy1([=] {
                    return parseType();
                }, Token::Comma);
            });
            return new (buffer) AppType(base, type);
        } else {
            return base;
        }
    } else if(token.type == Token::BraceL) {
        // Also handles unit type.
        return parseTupleType();
    } else if(token.type == Token::BracketL) {
        return parseArrayType();
    } else if(token.type == Token::ParenL) {
        eat();
        auto t = parseType();
        if(token.type == Token::ParenR) eat();
        else error("expected ')'"_buffer);

        return t;
    } else {
        error("expected a type"_buffer);
        return new (buffer) Type(Type::Error);
    }
}

SimpleType* Parser::parseSimpleType() {
    Id id = 0;
    if(token.type == Token::ConID) {
        id = token.data.id;
        eat();
    } else {
        error("expected type name"_buffer);
    }

    auto kind = maybeParens([=] {
        return sepBy1([=] {
            if(token.type == Token::VarID) {
                auto n = token.data.id;
                eat();
                return n;
            } else {
                error("expected an identifier"_buffer);
                return Id(0);
            }
        }, Token::Comma);
    });

    return new(buffer) SimpleType(id, kind);
}

Type* Parser::parseTupleType() {
    return node([=] {
        auto type = braces([=]() -> Type* {
            auto l = sepBy([=]() -> TupField {
                if(token.type == Token::VarID) {
                    auto gen = node([=]() -> GenType* {
                        auto name = token.data.id;
                        eat();
                        return new (buffer) GenType(name);
                    });

                    if(token.type == Token::opColon) {
                        eat();
                        return TupField(parseType(), gen->con, nullptr);
                    } else {
                        return TupField(gen, 0, nullptr);
                    }
                } else {
                    return TupField(parseType(), 0, nullptr);
                }
            }, Token::Comma, Token::BraceR);

            if(l) {
                return new (buffer) TupType(l);
            } else {
                return new (buffer) Type(Type::Unit);
            }
        });

        return type;
    });
}

Type* Parser::parseArrayType() {
    return node([=]() -> Type* {
        eat();
        auto from = parseType();
        if(token.type == Token::opArrowD) {
            eat();
            auto to = parseType();
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array type"_buffer);
            }

            return new(buffer) MapType(from, to);
        } else {
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array type"_buffer);
            }

            return new(buffer) ArrType(from);
        }
    });
}

Expr* Parser::parseTupleExpr() {
    return node([=] {
        return braces([=]() -> Expr* {
            bool firstQualified = false;
            if(token.type == Token::VarSym && token.data.id == hashId) {
                eat();
                firstQualified = true;
            }

            auto first = parseExpr();
            if(first->type != Expr::Var && firstQualified) {
                error("expected variable name"_buffer);
            }

            if(token.type == Token::opBar) {
                eat();
                auto args = sepBy1([=] {
                    return parseTupArg();
                }, Token::Comma);

                return new (buffer) TupUpdateExpr(first, args);
            } else {
                List<TupArg>* args;
                if(first->type == Expr::Assign) {
                    auto target = ((AssignExpr*)first)->target;
                    Id name = 0;
                    if(target->type == Expr::Var) {
                        name = ((VarExpr*)target)->name;
                    }

                    if(!name) {
                        error("tuple fields must be identifiers"_buffer);
                    }

                    args = list(TupArg(name, ((AssignExpr*)first)->value));
                } else if(firstQualified && first->type == Expr::Var) {
                    args = list(TupArg(((VarExpr*)first)->name, first));
                } else {
                    args = list(TupArg{0, first});
                }

                if(token.type == Token::Comma) {
                    eat();
                    args->next = sepBy1([=] {
                        return parseTupArg();
                    }, Token::Comma);
                }

                // Parent call checks the terminating '}'.
                return new (buffer) TupExpr(args);
            }
        });
    });
}

Expr* Parser::parseArrayExpr() {
    assert(token.type == Token::BracketL);
    eat();

    if(token.type == Token::BracketR) {
        // Empty array.
        eat();
        return new (buffer) ArrayExpr(nullptr);
    } else if(token.type == Token::opArrowD) {
        // Empty map.
        eat();
        if(token.type == Token::BracketR) {
            eat();
        } else {
            error("expected ']' after empty map"_buffer);
        }

        return new (buffer) MapExpr(nullptr);
    } else {
        auto first = parseExpr();
        if(token.type == Token::opArrowD) {
            eat();
            auto firstValue = parseExpr();
            if(token.type == Token::Comma) {
                eat();
                auto content = sepBy1([=] {
                    auto key = parseExpr();
                    if(token.type == Token::opArrowD) {
                        eat();
                    } else {
                        error("expected '=>' after map item key"_buffer);
                    }

                    auto value = parseExpr();
                    return MapArg(key, value);
                }, Token::Comma);

                auto l = list(MapArg(first, firstValue));
                l->next = content;

                if(token.type == Token::BracketR) {
                    eat();
                } else {
                    error("expected ']' after map end"_buffer);
                }

                return new (buffer) MapExpr(l);
            } else {
                if(token.type == Token::BracketR) {
                    eat();
                } else {
                    error("expected ']' after map end"_buffer);
                }

                return new (buffer) MapExpr(list(MapArg(first, firstValue)));
            }
        } else if(token.type == Token::Comma) {
            eat();
            auto content = sepBy1([=] {
                return parseExpr();
            }, Token::Comma);

            auto l = list(first);
            l->next = content;

            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array end"_buffer);
            }

            return new (buffer) ArrayExpr(l);
        } else {
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array end"_buffer);
            }

            return new (buffer) ArrayExpr(list(first));
        }
    }
}

Con Parser::parseCon() {
    /*
     * con	→	conid(type)
     *      |   conid tuptype
     *      |   conid
     */
    return node([=] {
        Id name;
        if(token.type == Token::ConID) {
            name = token.data.id;
            eat();
        } else {
            error("expected constructor name"_buffer);
            name = 0;
        }

        if(token.type == Token::ParenL) {
            auto content = parens([=]{return parseType();});
            return Con(name, content);
        } else if(token.type == Token::BraceL) {
            auto content = parseTupleType();
            return Con(name, content);
        } else if(token.type == Token::BracketL) {
            auto content = parseAType();
            return Con(name, content);
        } else {
            return Con(name, nullptr);
        }
    });
}

FieldPat Parser::parseFieldPat() {
    bool qualified = false;
    if(token.type == Token::VarSym && token.data.id == hashId) {
        eat();
        qualified = true;
    }

    if(qualified && token.type != Token::VarID) {
        error("expected variable name"_buffer);
    }

    if(token.type == Token::VarID) {
        auto varPat = node([=] {
            auto it = new (buffer) VarPat(token.data.id);
            eat();
            return it;
        });

        if(token.type == Token::opEquals) {
            eat();
            return FieldPat(varPat->var, parsePattern());
        } else {
            return FieldPat(qualified ? varPat->var : 0, varPat);
        }
    } else {
        return FieldPat(0, parsePattern());
    }
}

Pat* Parser::parseLeftPattern() {
    return node([=]() -> Pat* {
        if(token.type >= Token::FirstLiteral && token.type <= Token::LastLiteral) {
            auto p = new (buffer) LitPat(toLiteral(token));
            eat();
            return p;
        } else if(token.type == Token::kw_ || token.type == Token::kwElse) {
            eat();
            return new(buffer) Pat(Pat::Any);
        } else if(token.type == Token::VarID) {
            Id var = token.data.id;
            eat();
            if(token.type == Token::opAt) {
                eat();
                auto pat = parseLeftPattern();
                pat->asVar = var;
                return pat;
            } else {
                return new(buffer) VarPat(var);
            }
        } else if(token.type == Token::ParenL) {
            eat();
            auto pat = parsePattern();
            if(token.type == Token::ParenR) eat();
            else error("expected ')'"_buffer);

            return pat;
        } else if(token.type == Token::ConID) {
            // lpat can only contain a single constructor name.
            auto id = token.data.id;
            eat();
            return new(buffer) ConPat(id, nullptr);
        } else if(token.type == Token::BraceL) {
            auto expr = braces([=] {
                return sepBy1([=] {
                    return parseFieldPat();
                }, Token::Comma);
            });
            return new(buffer) TupPat(expr);
        } else if(token.type == Token::BracketL) {
            auto expr = brackets([=] {
                return sepBy([=] {
                    return parsePattern();
                }, Token::Comma, Token::BracketR);
            });

            return new(buffer) ArrayPat(expr);
        } else if(token.type == Token::opDotDot) {
            eat();
            Id var;
            if(token.type == Token::VarID) {
                var = token.data.id;
                eat();
            } else {
                var = 0;
                error("expected variable name"_buffer);
            }

            return new (buffer) RestPat(var);
        } else {
            error("expected pattern"_buffer);
            return new (buffer) Pat(Pat::Error);
        }
    });
}

Pat* Parser::parsePattern() {
    if(token.singleMinus) {
        eat();
        if(token.type == Token::Integer || token.type == Token::Float) {
            auto lit = toLiteral(token);
            if(token.type == Token::Integer) lit.i = -lit.i;
            else lit.f = -lit.f;
            eat();
            return new(buffer) LitPat(lit);
        } else {
            error("expected integer or float literal"_buffer);

            Literal lit;
            lit.type = Literal::Int;
            lit.i = 0;
            return new (buffer) LitPat(lit);
        }
    } else {
        bool allowRange = true;
        Pat* pat;
        if(token.type == Token::ConID) {
            auto id = token.data.id;
            eat();

            Pat* pats = nullptr;
            if(token.type == Token::ParenL) {
                auto fields = parens([=] {
                    return sepBy1([=] {
                        return parseFieldPat();
                    }, Token::Comma);
                });

                if(!fields || fields->next || fields->item.field) {
                    pats = new (buffer) TupPat(fields);
                } else {
                    pats = fields->item.pat;
                }
            } else if(token.type == Token::BraceL) {
                auto p = (TupPat*)parseLeftPattern();
                if(!p->fields || p->fields->next || p->fields->item.field) {
                    pats = p;
                } else {
                    pats = p->fields->item.pat;
                }
            }

            allowRange = pats == nullptr;
            pat = new (buffer) ConPat(id, pats);
        } else {
            pat = parseLeftPattern();
        }

        if(allowRange && token.type == Token::opDotDot) {
            eat();
            auto to = parseLeftPattern();
            return new (buffer) RangePat(pat, to);
        } else {
            return pat;
        }
    }
}

Attribute Parser::parseAttribute() {
    return node([=] {
        if(token.type == Token::opAt) {
            eat();
        } else {
            error("expected '@'"_buffer);
        }

        Id name = 0;
        if(token.type == Token::VarID || token.type == Token::ConID) {
            name = token.data.id;
            eat();
        } else {
            error("expected identifier or type name"_buffer);
        }

        if(token.type == Token::ParenL) {
            auto args = parens([=] {
                return sepBy([=] {
                    return parseTupArg();
                }, Token::Comma, Token::ParenR);
            });

            return Attribute{name, args};
        } else if(token.type == Token::BraceL) {
            auto expr = (TupExpr*)parseTupleExpr();
            return Attribute{name, expr->args};
        } else {
            return Attribute{name, nullptr};
        }
    });
}

List<Attribute>* Parser::parseAttributes() {
    List<Attribute>* attributes = nullptr;
    while(token.type == Token::opAt) {
        attributes = list(parseAttribute(), attributes);

        if(token.type == Token::EndOfStmt) {
            eat();
        }
    }

    return attributes;
}

void Parser::error(StringBuffer text, Node* node) {
    Node where;
    where.sourceStart.line = (U16)token.startLine;
    where.sourceStart.column = (U16)token.startColumn;
    where.sourceStart.offset = (U16)token.startOffset;
    where.sourceEnd.line = (U16)token.endLine;
    where.sourceEnd.column = (U16)token.endColumn;
    where.sourceEnd.offset = (U16)token.endOffset;
    where.sourceModule = module.name;

    diag.error(text, node ? node : &where, {this->text, stringLength(this->text)});
}
