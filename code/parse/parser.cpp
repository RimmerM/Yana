#include "parser.h"

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

void Parser::parseModule() {
    withLevel([=] {
        while(token.type == Token::EndOfStmt) {
            while(token.type == Token::EndOfStmt) {
                eat();
            }

            if(token.type == Token::kwImport) {
                parseImport();
            } else if(token.type == Token::kwInfixL || token.type == Token::kwInfixR) {
                parseFixity();
            } else {
                module.decls << parseDecl();
            }
        }

        // Dummy return value to withLevel.
        return true;
    });
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
        error("expected symbol name");
        name = 0;
    }

    List<Id>* include = maybeParens([=] {
        return sepBy1([=] {
            Id included;
            if(token.type == Token::VarID || token.type == Token::ConID) {
                included = token.data.id;
                eat();
            } else {
                error("expected symbol name");
                included = 0;
            }
            return included;
        }, Token::Comma);
    });

    List<Id>* exclude = nullptr;
    if(token.type == Token::VarID && token.data.id == hidingId) {
        eat();
        exclude = parens([=] {
            return sepBy1([=] {
                Id hiddenName;
                if(token.type == Token::VarID || token.type == Token::ConID) {
                    hiddenName = token.data.id;
                    eat();
                } else {
                    error("expected symbol name");
                    hiddenName = 0;
                }
                return hiddenName;
            }, Token::Comma);
        });
    }

    Id asName = 0;
    if(token.type == Token::VarID && token.data.id == asId) {
        eat();

        if(token.type == Token::ConID) {
            asName = token.data.id;
            eat();
        } else {
            error("expected identifier");
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
            error("expected operator precedence");
            precedence = 9;
        }

        auto op = parseQop();
        auto id = op ? op->name : 0;

        return new (buffer) Fixity{id, precedence, kind};
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
        return parseFunDecl();
    } else if(token.type == Token::kwClass) {
        return parseClassDecl();
    } else if(token.type == Token::kwInstance) {
        return parseInstanceDecl();
    } else {
        return new (buffer) StmtDecl(parseExpr());
    }
}

Decl* Parser::parseFunDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwFn);
        eat();

        Id name;
        if(token.type == Token::VarID) {
            name = token.data.id;
            eat();
        } else {
            name = 0;
            error("expected function name");
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

        Expr *body;
        if(token.type == Token::opEquals) {
            body = node([=]() -> Expr* {
                eat();
                auto expr = parseExpr();
                if(token.type == Token::kwWhere) {
                    eat();
                    auto decls = list(parseVarDecl());
                    decls->next = list(expr);
                    return new(buffer) MultiExpr(decls);
                } else {
                    return expr;
                }
            });
        } else {
            body = parseBlock(true);
        }

        return new(buffer) FunDecl(name, body, args, ret);
    });
}

Decl* Parser::parseDataDecl() {
    return node([=]() -> DataDecl* {
        assert(token.type == Token::kwData);
        eat();

        auto type = parseSimpleType();
        if(token.type == Token::opEquals) {
            eat();
        } else {
            error("expected '=' after type name");
        }

        auto cons = sepBy1([=] {
            return node([=] {return parseCon();});
        }, Token::opBar);

        return new (buffer) DataDecl(type, cons);
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
            error("expected '='");
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

        Id name = 0;
        if(token.type == Token::VarID) {
            name = token.data.id;
            eat();
        } else {
            error("expected identifier");
        }

        // A normal function type looks exactly like a function declaration when directly after the name.
        if(token.type == Token::opColon) {
            eat();
        } else if(!isFun) {
            error("expected ':'");
        }

        auto type = parseType();

        Id from = 0;
        if(token.type == Token::VarID && token.data.id == fromId) {
            eat();
            if(token.type == Token::String) {
                from = token.data.id;
                eat();
            } else {
                error("expected a string");
            }
        }

        Id importName = 0;
        if(token.type == Token::VarID && token.data.id == asId) {
            eat();
            if(token.type == Token::VarID) {
                importName = token.data.id;
                eat();
            } else {
                error("expected an identifier");
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
            error("expected ':' after class declaration");
        }

        auto decls = withLevel([=] {
            return sepBy([=] {
                return parseDecl();
            }, Token::EndOfStmt, Token::EndOfBlock);
        });

        return new(buffer) ClassDecl(type, decls);
    });
}

Decl* Parser::parseInstanceDecl() {
    return node([=]() -> Decl* {
        assert(token.type == Token::kwInstance);
        eat();

        auto type = parseSimpleType();
        if(token.type == Token::opColon) {
            eat();
        } else {
            error("expected ':' after instance declaration");
        }

        auto decls = withLevel([=] {
            return sepBy([=] {
                return parseDecl();
            }, Token::EndOfStmt, Token::EndOfBlock);
        });

        return new(buffer) InstanceDecl(type, decls);
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
            error("expected ':'");
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

    if(!list) {
        error("expected an expression");
        return nullptr;
    } else if(!list->next) {
        return list->item;
    } else {
        return new(buffer) MultiExpr(list);
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
    if(!expr) return nullptr;

    if(token.type == Token::opColonColon) {
        eat();
        if(auto type = parseType()) {
            return new(buffer) CoerceExpr(expr, type);
        } else {
            return nullptr;
        }
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
    if(auto lhs = parsePrefixExpr()) {
        if(token.type == Token::opEquals) {
            eat();
            auto rhs = parseExpr();
            if(!rhs) error("Expected an expression after assignment.");

            return new(buffer) AssignExpr(lhs, rhs);
        } else if(token.type == Token::VarSym || token.type == Token::Grave) {
            // Binary operator.
            auto op = parseQop();
            auto rhs = parseInfixExpr();
            if(!rhs) error("Expected a right-hand side for a binary operator.");

            return new(buffer) InfixExpr(op, lhs, rhs);
        } else {
            // Single expression.
            return lhs;
        }
    } else {
        error("expected an expression.");
        return nullptr;
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
        return new (buffer) PrefixExpr(op, expr);
    } else {
        return parseLeftExpr();
    }
}

Expr* Parser::parseLeftExpr() {
    return node([=]() -> Expr* {
        if(token.type == Token::kwLet) {
            eat();
            return parseVarDecl();
        } else if(token.type == Token::kwMatch) {
            return parseCaseExpr();
        } else if(token.type == Token::kwIf) {
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
                        else error("expected '->' after if condition");

                        auto then = parseExpr();
                        return IfCase(cond, then);
                    }, Token::EndOfStmt);
                });
                return new(buffer) MultiIfExpr(list);
            } else {
                auto cond = parseExpr();

                // Allow statement ends within an if-expression to allow then/else with the same indentation as if.
                if(token.type == Token::EndOfStmt) eat();
                if(token.type == Token::kwThen) {
                    eat();

                    Expr* expr;
                    if(token.type == Token::opColon) {
                        expr = parseBlock(false);
                    } else {
                        expr = parseExpr();
                    }

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
                        return new (buffer) IfExpr(cond, expr, nullptr);
                    }
                } else {
                    error("Expected 'then' after if-expression.");
                }
            }
        } else if(token.type == Token::kwWhile) {
            eat();
            if(auto cond = parseExpr()) {
                if(auto loop = parseBlock(false)) {
                    return new(buffer) WhileExpr(cond, loop);
                } else {
                    error("Expected expression after ':'");
                }
            } else {
                error("Expected expression after 'while'");
            }
        } else if(token.type == Token::kwReturn) {
            eat();
            auto body = parseExpr();
            return new (buffer) RetExpr(body);
        } else {
            return parseAppExpr();
        }

        return nullptr;
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
    if(auto exp = parseExpr()) {
        if(token.type == Token::opColon) {
            eat();
            auto alts = withLevel([=] {
                return sepBy1([=] {
                    return parseAlt();
                }, Token::EndOfStmt);
            });
            return new(buffer) CaseExpr(exp, alts);
        } else {
            error("Expected ':' after match-expression.");
        }
    } else {
        error("expected an expression after 'match'");
    }

    return nullptr;
}

Expr* Parser::parseBaseExpr() {
    if(token.type == Token::ParenL) {
        return node([=]() -> Expr* {
            eat();
            if(token.type == Token::ParenR) {
                eat();
                return new (buffer) FunExpr(nullptr, parseBlock(false));
            } else {
                auto e = parseExpr();
                if(token.type == Token::ParenR) {
                    eat();
                    return new (buffer) NestedExpr(e);
                } else {
                    return new (buffer) Expr(Expr::Error);
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
                        return parseExpr();
                    }, Token::Comma, Token::ParenR);
                });

                return new (buffer) ConExpr(type, args);
            } else if(token.type == Token::BraceL) {
                return new (buffer) ConExpr(type, list(parseTupleExpr()));
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
            error("expected an expression");
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
                error("expected end of string format after this expression.");
            }

            eat();
            assert(token.type == Token::String);
            p->next = list(FormatChunk{token.data.id, expr});
            p = p->next;
            eat();
        }

        return new(buffer) FormatExpr(l);
    } else {
        return new(buffer) LitExpr(toStringLiteral(string));
    }
}

TupArg Parser::parseTupArg() {
    if(token.type == Token::VarID) {
        auto varExpr = node([=]() -> VarExpr* {
            auto id = token.data.id;
            eat();
            return new (buffer) VarExpr(id);
        });

        if(token.type == Token::opEquals) {
            eat();
            return TupArg(varExpr->name, parseExpr());
        } else {
            return TupArg(0, varExpr);
        }
    } else {
        return TupArg{0, parseExpr()};
    }
}

Arg Parser::parseArg(bool requireType) {
    Id name = 0;
    if(token.type == Token::VarID) {
        name = token.data.id;
        eat();
    } else {
        error("expected parameter name");
    }

    Type* type = nullptr;
    if(requireType || token.type == Token::opColon) {
        if(token.type == Token::opColon) eat();
        type = parseType();
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
                error("expected identifier");
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

Expr* Parser::parseVarDecl() {
    // Parse one or more declarations, separated as statements.
    auto list = withLevel([=] {
        return sepBy1([=] {
            return parseDeclExpr();
        }, Token::EndOfStmt);
    });

    if(!list) {
        error("expected declaration after 'let'");
        return nullptr;
    } else if(!list->next) {
        return list->item;
    } else {
        return new(buffer) MultiExpr(list);
    }
}

Expr* Parser::parseDeclExpr() {
    bool isRef = false;
    if(token.type == Token::VarSym && token.data.id == refId) {
        eat();
        isRef = true;
    }

    if(token.type == Token::VarID) {
        auto id = token.data.id;
        eat();
        if(token.type == Token::opEquals) {
            eat();
            if(auto expr = parseExpr()) {
                return new(buffer) DeclExpr(id, expr, isRef);
            } else {
                error("Expected expression.");
            }
        } else {
            return new(buffer) DeclExpr(id, nullptr, isRef);
        }
    } else {
        error("expected identifier");
    }

    return nullptr;
}

Alt Parser::parseAlt() {
    /*
     * alt	→	pat -> exp [where decls]
     * 		|	pat gdpat [where decls]
     * 		|		    					(empty alternative)
     */
    auto pat = parsePattern();
    if(token.type == Token::opArrowR) {
        eat();
    } else {
        error("expected '->'");
    }

    auto exp = parseExpr();
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
                return new (buffer) VarExpr(id);
            }
        }
    }

    return nullptr;
}

VarExpr* Parser::parseQop() {
    /*
     * qop	→	qvarsym | `qvarid`
     */
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
                error("Expected '`' after operator identifier");
            }

            return new (buffer) VarExpr(id);
        }
    }

    error("Expected an operator");
    return nullptr;
}

Type* Parser::parseType() {
    return node([=]() -> Type* {
        List<ArgDecl>* args = maybeParens([=] {
            return sepBy([=] {
                return parseTypeArg();
            }, Token::Comma, Token::ParenR);
        });

        if(!args) {
            return parseAType();
        } else if(token.type == Token::opArrowR) {
            eat();
            return new (buffer) FunType(args, parseAType());
        } else {
            auto arg = args->next ? nullptr : &args->item;
            if(arg && !arg->name) {
                return arg->type;
            } else {
                error("expected '=>' after function type args");
                return new (buffer) Type(Type::Error);
            }
        }
    });
}

Type* Parser::parseAType() {
    if(token.type == Token::VarSym && token.data.id == ptrId) {
        auto type = parseAType();
        return new (buffer) PtrType(type);
    } else if(token.type == Token::ConID) {
        auto id = token.data.id;
        eat();
        return new(buffer) ConType(id);
    } else if(token.type == Token::VarID) {
        auto id = token.data.id;
        eat();
        return new(buffer) GenType(id);
    } else if(token.type == Token::BraceL) {
        // Also handles unit type.
        return parseTupleType();
    } else if(token.type == Token::BracketL) {
        eat();
        auto from = parseType();
        if(token.type == Token::opArrowD) {
            eat();
            auto to = parseType();
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array type");
            }

            return new (buffer) MapType(from, to);
        } else {
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array type");
            }

            return new (buffer) ArrType(from);
        }
    } else if(token.type == Token::ParenL) {
        eat();
        auto t = parseType();
        if(token.type == Token::ParenR) eat();
        else error("expected ')'");

        return t;
    }

    error("Expected a type.");
    return nullptr;
}

SimpleType* Parser::parseSimpleType() {
    if(token.type == Token::ConID) {
        auto id = token.data.id;
        eat();

        auto kind = maybeParens([=] {
            return sepBy1([=] {
                if(token.type == Token::VarID) {
                    auto n = token.data.id;
                    eat();
                    return n;
                } else {
                    error("expected an identifier");
                    return Id(0);
                }
            }, Token::Comma);
        });

        return new(buffer) SimpleType(id, kind);
    } else {
        error("expected type name");
    }

    return nullptr;
}

Type* Parser::parseTupleType() {
    return node([=] {
        auto type = braces([=]() -> Type* {
            auto l = sepBy1([=]() -> TupField {
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
            }, Token::Comma);

            if(l) return new (buffer) TupType(l);
            else return new (buffer) Type(Type::Unit);
        });

        if(!type) error("expected one or more tuple fields");
        return type;
    });
}

Expr* Parser::parseTupleExpr() {
    return node([=] {
        return braces([=]() -> Expr* {
            auto first = parseExpr();
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
                        name = ((VarExpr*)target->type)->name;
                    }

                    if(!name) {
                        error("tuple fields must be identifiers");
                    }

                    args = list(TupArg(name, ((AssignExpr*)first)->value));
                } else {
                    args = list(TupArg{0, first});
                }

                args->next = sepBy1([=] {
                    return parseTupArg();
                }, Token::Comma);

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
            error("expected ']' after empty map");
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
                        error("expected '=>' after map item key");
                    }

                    auto value = parseExpr();
                    return MapArg(key, value);
                }, Token::Comma);

                auto l = list(MapArg(first, firstValue));
                l->next = content;

                if(token.type == Token::BracketR) {
                    eat();
                } else {
                    error("expected ']' after map end");
                }

                return new (buffer) MapExpr(l);
            } else {
                if(token.type == Token::BracketR) {
                    eat();
                } else {
                    error("expected ']' after map end");
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
                error("expected ']' after array end");
            }

            return new (buffer) ArrayExpr(l);
        } else {
            if(token.type == Token::BracketR) {
                eat();
            } else {
                error("expected ']' after array end");
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
            error("expected constructor name");
            name = 0;
        }

        if(token.type == Token::ParenL) {
            auto content = parens([=]{return parseType();});
            return Con(name, content);
        } else if(token.type == Token::BraceL) {
            auto content = parseTupleType();
            return Con(name, content);
        } else {
            return Con(name, nullptr);
        }
    });
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
            else error("expected ')'");

            return pat;
        } else if(token.type == Token::ConID) {
            // lpat can only contain a single constructor name.
            auto id = token.data.id;
            eat();
            return new(buffer) ConPat(id, nullptr);
        } else if(token.type == Token::BraceL) {
            auto expr = braces([=] {
                return sepBy1([=]() -> FieldPat {
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
                            return FieldPat(0, varPat);
                        }
                    } else {
                        return FieldPat(0, parsePattern());
                    }
                }, Token::Comma);
            });
            return new(buffer) TupPat(expr);
        } else {
            error("expected pattern");
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
            error("expected integer or float literal");
            return nullptr;
        }
    } else if(token.type == Token::ConID) {
        auto id = token.data.id;
        eat();

        auto pat = parseLeftPattern();
        return new(buffer) ConPat(id, pat);
    } else {
        return parseLeftPattern();
    }
}

void Parser::error(const char* text) {

}
