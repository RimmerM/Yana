#include "complete.h"
#include "resolve_internal.h"
#include "name.h"
#include "../intrinsic/primitive.h"

/*
 * What is in scope at the cursor, collected once - Implementation-Tooling.md §8.
 *
 * Everything below runs at the moment the resolver reaches the sentinel and never afterwards, which is
 * what makes it cheap enough to do eagerly: the enclosing scope, the expected type and the receiver are
 * live right then, and keeping them past the capture would mean keeping the resolver itself.
 */

namespace mir {

namespace {

/*
 * Ranking.
 *
 * A rank rather than a score: a client sorts by `sortText` and filters by the typed prefix, so what the
 * server owes it is a coarse, stable grouping. Lower sorts first.
 */
enum Rank: U8 {
    // What this position asked for - §8.2's reason for the expected type being worth having at all.
    RankExpected = 0,

    // The names written nearest the cursor - this body's, then this module's.
    RankLocal = 1,
    RankModule = 2,

    // Everything reached through an import, which for any program is most of the list.
    RankImported = 3,
};

// A declaration and its instantiations are one answer here: a constructor of `Maybe(a)` is exactly what
// a `Maybe(I32)` position wants. That is the only widening - a name whose type merely *could* be
// converted is how a ranked list becomes an unranked one.
DeclPtr<Type> declarationOf(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Record) return type;

    auto& record = *(RecordType*)base[type];
    return record.instanceOf ? asBase<Type>(record.instanceOf) : type;
}

bool fitsExpected(DeclBase base, DeclPtr<Type> type, DeclPtr<Type> expected) {
    if(!type || !expected) return false;
    return declarationOf(base, type) == declarationOf(base, expected);
}

struct Collector {
    Resolver& resolver;
    CompletionRequest& request;

    // The names already offered, so that a binding shadowing an outer one - or an import offering what
    // this module also declares - is one item rather than two. Nearest first is what makes that right.
    // Keyed on the qualifier as well: `Mode.Fast` and a bare `Fast` are two spellings of two things.
    Array<StringId> seen;
    Array<StringId> seenQualifiers;

    // The same for the *name* half of a `name: value` pair, kept apart: a parameter called `count` and
    // a local called `count` are two different things to write at an argument position.
    Array<StringId> seenNaming;

    // The members of every instance, which are reached by selection and have no name to offer.
    HashMap<U32, bool> instanceMembers;

    bool take(StringId name, bool naming, StringId qualifier = {}) {
        if(name == StringId()) return false;

        if(naming) {
            for(auto existing: seenNaming) {
                if(existing == name) return false;
            }

            seenNaming.push(name);
            return true;
        }

        for(Size i = 0; i < seen.size(); i++) {
            if(seen[i] == name && seenQualifiers[i] == qualifier) return false;
        }

        seen.push(name);
        seenQualifiers.push(qualifier);
        return true;
    }

    void add(const Symbol& symbol, DeclPtr<Type> type, StringId qualifier, U8 rank, bool naming = false) {
        if(!take(symbol.name, naming, qualifier)) return;

        CompletionItem item;
        item.symbol = symbol;
        item.type = type;
        item.qualifier = qualifier;
        item.naming = naming;
        item.rank = fitsExpected(resolver.decl(), type, request.expected) ? U8(RankExpected) : rank;

        request.items.push(item);
    }
};

void begin(Resolver& resolver, CompletionRequest& request, Collector& collector) {
    request.captured = true;
    request.module = resolver.here();
    request.function = resolver.currentFunction ? resolver.currentFunction - *resolver.program.decl
                                                : DeclPtr<Function>(nullptr);

    auto base = resolver.decl();
    for(auto module: resolver.program.modules) {
        for(auto instance: base[module]->instances.contents(base)) {
            for(auto member: base[instance]->members.contents(base)) {
                if(member) *collector.instanceMembers.add(member.offset).value = true;
            }
        }
    }
}

// An operator is a name a program writes between its operands rather than one it types to call, and a
// completion list is for the second.
bool operatorNamed(Context& context, StringId name) {
    auto& identifier = context.find(name);
    if(!identifier.textLength) return true;

    auto c = identifier.text[0];
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}

// Whether a function is one a name written here could reach by that name - not a specialization, an
// instance member, a lifted lambda, a generated body or a class member (which comes with its class).
bool offerable(Collector& into, DeclPtr<Function> pointer) {
    auto& resolver = into.resolver;
    auto& function = *resolver.decl()[pointer];

    if(function.klass || function.specializes || function.environment) return false;
    if(function.provided != intrinsic::Provided::Count) return false;
    if(isCompilerName(resolver.context, function.name)) return false;
    if(operatorNamed(resolver.context, function.name)) return false;

    return !into.instanceMembers.get(pointer.offset);
}

// The leading segment of a name, which is what an import's include and exclude lists name - so that
// `hiding (String)` hides `String.reserve` along with the type, exactly as `search` reads it.
StringId leadingSegment(Context& context, StringId name) {
    return context.find(name).getHash(0);
}

// The last segment of a namespaced name, as a name in its own right - what a dot-call writes - or
// nothing for a name with no namespace. From the text, since a declared name is interned by its whole
// spelling and the segment count it carries is the spelling's business rather than the namespace's.
StringId lastSegment(Context& context, StringId name) {
    auto& identifier = context.find(name);

    for(auto i = identifier.textLength; i > 0; i--) {
        if(identifier.text[i - 1] != '.') continue;
        return context.addUnqualifiedName(identifier.text + i, identifier.textLength - i);
    }

    return StringId();
}

/*
 * Every module a name written here can reach, with what an item from it has to be written under.
 *
 * The module itself first and then its imports, in scope in the file being read - the order `search`
 * walks them, and so the order the shadowing rule wants. What `offer` is handed is the import that
 * reached the module, or null for the module's own declarations: visibility is asked of imports only,
 * because a private declaration of *this* module is one its bodies may name.
 */
template<class Offer>
void eachVisibleModule(Resolver& resolver, Offer&& offer) {
    auto base = resolver.decl();
    auto from = resolver.here();
    auto& module = *base[from];

    offer(from, (Import*)nullptr, StringId(), U8(RankModule));

    for(auto& import: module.imports.contents(base)) {
        if(!import.module || import.module == from) continue;
        if(!import.inScope(module.activeFile)) continue;

        offer(import.module, &import, import.qualified ? import.localName : StringId(),
              U8(RankImported));
    }
}

// Whether an import offers one declaration - both halves of visibility, exactly as `search` asks them.
bool importOffers(Resolver& resolver, Import* import, StringId name, bool exported) {
    if(!import) return true;
    return exported && import->visible(resolver.decl(), leadingSegment(resolver.context, name));
}

/*
 * What kind of position the cursor is in, which is what decides which tables are offered at all.
 *
 * `Values` is an expression: constructors, functions and globals, and a type only where something is
 * reached *through* it with a dot - see `namespaceHeads`. `Constructors` is a pattern. `Types` is a
 * type position - after `::`, in a parameter, a field or a result - where the items are types and
 * aliases and nothing a value could be.
 */
enum class Offered: U8 { Values, Constructors, Types };

// The text of a name up to its last dot, interned - `Point` for `Point.span` - or nothing for a name
// with no namespace. From the text for `lastSegment`'s reason.
StringId namespaceHead(Context& context, StringId name) {
    auto& identifier = context.find(name);

    for(auto i = identifier.textLength; i > 0; i--) {
        if(identifier.text[i - 1] != '.') continue;
        return context.addUnqualifiedName(identifier.text, i - 1);
    }

    return StringId();
}

/*
 * The type names a dot reaches something through, in any visible module: the head of every
 * namespaced function (`fn String.push` makes `String` one), and every record whose constructors are
 * `data qualified` and are written `Mode.Fast`.
 *
 * This is what a type name in an expression is *for*. A type is not a value, so offering one where a
 * value is being written is offering a name that does not resolve - unless what the author means to
 * type next is the dot. The constructor a record shares its name with is offered as the constructor,
 * which is the other half of the same answer.
 */
void namespaceHeads(Resolver& resolver, Array<StringId>& into) {
    auto& program = resolver.program;
    auto base = resolver.decl();

    auto add = [&](StringId name) {
        if(!name) return;
        for(auto existing: into) if(existing == name) return;
        into.push(name);
    };

    eachVisibleModule(resolver, [&](DeclPtr<mir::Module> owner, Import*, StringId, U8) {
        auto& module = *base[owner];

        for(auto pointer: module.functions.contents(base)) {
            add(namespaceHead(resolver.context, base[pointer]->name));
        }

        for(auto pointer: module.records.contents(base)) {
            auto& record = *base[pointer];
            if(record.tuple || record.instanceOf) continue;

            for(U16 i = 0; i < U16(record.cons.size()); i++) {
                if(program.findConstructor(owner, record.cons.get(base, i).name).record == pointer) continue;

                add(record.name);
                break;
            }
        }
    });
}

/*
 * §8.1's first kind: everything a name written here could refer to.
 *
 * The order the tables are walked in decides a name two of them hold - `data Point {..}` declares both
 * a type and its constructor - so constructors come before types. A constructor completes to
 * `Point {x: , y: }` where a type completes to a name that does not resolve.
 *
 * A `data qualified` constructor is offered under its type's name - `Mode.Fast` - since that is the
 * only way it is written. It carries the type as its qualifier, which is what the server's filter
 * compares with a typed `Mode.` and what the label is written under when nothing was typed yet.
 * Only through an unqualified import: a constructor reached as `U.Mode.Fast` would need a qualifier
 * of two segments, and is rare enough to leave out.
 */
void collectVisible(Collector& into, Offered position = Offered::Values) {
    auto& resolver = into.resolver;
    auto& program = resolver.program;
    auto base = resolver.decl();

    Array<StringId> heads;
    if(position == Offered::Values) namespaceHeads(resolver, heads);

    auto reachedThrough = [&](StringId name) {
        for(auto head: heads) if(head == name) return true;
        return false;
    };

    eachVisibleModule(resolver, [&](DeclPtr<mir::Module> owner, Import* import, StringId qualifier,
                                    U8 rank) {
        auto& module = *base[owner];

        auto offer = [&](const Symbol& symbol, DeclPtr<Type> type, bool exported) {
            if(!importOffers(resolver, import, symbol.name, exported)) return;
            into.add(symbol, type, qualifier, rank);
        };

        if(position != Offered::Types) {
            for(auto pointer: module.records.contents(base)) {
                auto& record = *base[pointer];
                if(record.tuple || record.instanceOf) continue;

                for(U16 i = 0; i < U16(record.cons.size()); i++) {
                    auto name = record.cons.get(base, i).name;
                    auto symbol = constructorSymbol(base, ConstructorRef { pointer, i });
                    auto exported = exportedSymbol(base, pointer);

                    if(program.findConstructor(owner, name).record == pointer) {
                        offer(symbol, asBase<Type>(pointer), exported);
                    } else if(!qualifier && importOffers(resolver, import, record.name, exported)) {
                        into.add(symbol, asBase<Type>(pointer), record.name, rank);
                    }
                }
            }
        }

        if(position == Offered::Constructors) return;

        if(position == Offered::Values) {
            for(auto pointer: module.functions.contents(base)) {
                if(!offerable(into, pointer)) continue;

                auto& function = *base[pointer];
                offer(functionSymbol(base, pointer), function.result.type, function.exported);
            }

            for(auto pointer: module.globals.contents(base)) {
                offer(globalSymbol(program, pointer, owner), base[pointer]->occurrence.type,
                      base[pointer]->exported);
            }
        }

        for(auto pointer: module.records.contents(base)) {
            auto& record = *base[pointer];
            if(record.tuple || record.instanceOf) continue;
            if(position == Offered::Values && !reachedThrough(record.name)) continue;

            offer(typeSymbol(base, asBase<Type>(pointer)), nullptr, exportedSymbol(base, pointer));
        }

        for(auto pointer: program.aliases) {
            if(base[pointer]->module != owner) continue;
            if(position == Offered::Values && !reachedThrough(base[pointer]->name)) continue;

            offer(aliasSymbol(base, pointer), nullptr, base[pointer]->exported);
        }

        // A class is neither a value nor a type, so it is offered for the members written in its
        // own namespace and nowhere else. A member is exported with its class, which is the rule
        // `exportedSymbol` states for one.
        if(position != Offered::Values) return;

        for(auto pointer: program.classes) {
            auto& klass = *base[pointer];
            if(klass.module != owner) continue;

            if(intrinsic::isTeardownRole(klass.role)) continue;

            for(auto member: klass.members.contents(base)) {
                offer(functionSymbol(base, member), base[member]->result.type, klass.exported);
            }
        }
    });
}

/*
 * §8.1's second kind: the bindings of the enclosing body, innermost first - which is both the shadowing
 * rule and the ranking.
 *
 * Down past a lambda's floor to the body it was written in, because a name the lambda does not bind may
 * still belong to that one, and naming it is what makes it a capture. Not past a body built on demand
 * inside another, whose names belong to a different declaration altogether.
 */
void collectBindings(Collector& into) {
    auto& resolver = into.resolver;
    if(!resolver.currentFunction) return;

    auto floor = resolver.bindingFloor;
    for(auto i = resolver.lambdaFloor; i < resolver.lambdaFrames.size(); i++) {
        auto outer = resolver.lambdaFrames[i].outerFloor;
        if(outer < floor) floor = outer;
    }

    auto function = resolver.currentFunction - *resolver.program.decl;

    for(auto i = resolver.bindings.size(); i > floor; i--) {
        auto& binding = resolver.bindings[i - 1];
        if(!binding.local || binding.name == StringId()) continue;
        if(isCursorSentinel(resolver.context, binding.name)) continue;
        if(isCompilerName(resolver.context, binding.name)) continue;

        into.add(localSymbol(resolver.decl(), *resolver.program.mir, function, *binding.local),
                 binding.local->occurrence.type, StringId(), RankLocal);
    }
}

// The index of the first parameter a call writes, or the count where there is none.
U32 firstWritten(Function& function) {
    for(U32 i = 0; i < U32(function.args.size()); i++) {
        if(function.writesArg(i)) return i;
    }

    return U32(function.args.size());
}

/*
 * Whether a function whose first parameter is `parameter` accepts this receiver - which is what a
 * dot-call `x.f(y)` as `f(x, y)` needs of it.
 *
 * The one-way matcher a call site infers type arguments with, so a generic's variable binds to
 * whatever the receiver is - `fn twice(v: a)` really is callable on everything - and then the class
 * requirements the signature states are asked at what was bound: `fn (Chunked(c, a)) contains(xs: c)`
 * binds `c` to anything and needs `Chunked(c, a)` to compile, so offering it after every `.` would be
 * offering what does not compile. A requirement whose arguments the receiver left open is not judged,
 * which over-offers rather than under-offers - the safe direction for a list the author types past.
 */
bool acceptsReceiver(Resolver& resolver, DeclPtr<GenContext> gen, DeclPtr<Type> parameter,
                     DeclPtr<Type> receiver) {
    auto base = resolver.decl();
    auto& program = resolver.program;
    if(!parameter || !receiver) return false;

    if(!isGeneric(base, gen)) return parameter == receiver;

    SmallArray<DeclPtr<Type>, 4> bound;
    for(Size i = 0; i < base[gen]->variables.size(); i++) bound.push(nullptr);

    if(!matchType(base, program.types, gen, bound, parameter, receiver)) return false;

    auto allBound = true;
    for(Size i = 0; i < bound.size(); i++) allBound = allBound && bound[i];

    Substitution with { gen, bound.pointer(), bound.size() };

    for(auto requirement: base[gen]->classes.contents(base)) {
        auto& klass = *base[requirement.klass];
        SmallArray<DeclPtr<Type>, 4> arguments;
        auto judged = true;
        U16 position = 0;

        for(auto argument: requirement.args.contents(base)) {
            auto at = position++;
            DeclPtr<Type> answer = nullptr;

            if(base[argument]->kind == Type::Variable && ((VariableType*)base[argument])->owner == gen) {
                answer = bound[((VariableType*)base[argument])->index];
            } else if(isConcreteType(base, argument)) {
                answer = argument;
            } else if(allBound) {
                answer = substituteType(resolver.context, program, argument, with);
                if(answer && !isConcreteType(base, answer)) answer = nullptr;
            }

            // An open position the class *determines* is what selection fills in itself - the
            // table's own protocol. An open position that decides is not a question it may be asked.
            if(!answer && !klass.isDetermined(at)) {
                judged = false;
                break;
            }

            arguments.push(answer);
        }

        if(!judged) continue;

        InstanceMatch found;
        if(!selectInstance(resolver.context, program, requirement.klass, arguments.pointer(),
                           arguments.size(), kNullLocation, false, found)) {
            return false;
        }
    }

    return true;
}

/*
 * §8.1's third kind, second half: every visible function a dot-call could reach.
 *
 * A namespaced name reaches a dot only through the type that owns it, and under its last segment -
 * `s.` offers `reserve` for `String.reserve` - and every other namespaced name is skipped: `Hmac.tag`
 * takes an `Int` first and would otherwise be offered on every integer under a name no call could use.
 *
 * A class member is reachable exactly when the program implements its class at what binding the first
 * parameter decided, and the open positions go to selection as nulls - the table's own protocol for a
 * class keyed on more parameters than the caller decides.
 */
void collectDotCallables(Collector& into, DeclPtr<Type> receiver) {
    auto& resolver = into.resolver;
    auto& context = resolver.context;
    auto& program = resolver.program;
    auto base = resolver.decl();

    eachVisibleModule(resolver, [&](DeclPtr<mir::Module> owner, Import* import, StringId qualifier,
                                    U8 rank) {
        auto& module = *base[owner];

        auto offer = [&](Symbol symbol, DeclPtr<Type> type, bool exported, StringId label) {
            if(!importOffers(resolver, import, symbol.name, exported)) return;
            if(label != StringId()) symbol.name = label;
            into.add(symbol, type, qualifier, rank);
        };

        for(auto pointer: module.functions.contents(base)) {
            if(!offerable(into, pointer)) continue;

            auto& function = *base[pointer];
            auto first = firstWritten(function);
            if(first >= function.args.size()) continue;

            auto label = lastSegment(context, function.name);
            if(label != StringId() && namespacedName(context, program, receiver, label) != function.name) {
                continue;
            }

            auto parameter = base[function.args.get(base, first)]->occurrence.type;
            if(!acceptsReceiver(resolver, function.gen, parameter, receiver)) continue;

            offer(functionSymbol(base, pointer), function.result.type, function.exported, label);
        }

        for(auto pointer: program.classes) {
            auto& klass = *base[pointer];
            if(klass.module != owner || intrinsic::isTeardownRole(klass.role)) continue;

            for(auto member: klass.members.contents(base)) {
                auto& signature = *base[member];
                auto first = firstWritten(signature);
                if(first >= signature.args.size()) continue;

                SmallArray<DeclPtr<Type>, 4> bound;
                for(Size i = 0; i < base[klass.gen]->variables.size(); i++) bound.push(nullptr);

                auto parameter = base[signature.args.get(base, first)]->occurrence.type;
                if(!matchType(base, program.types, klass.gen, bound, parameter, receiver)) continue;

                InstanceMatch found;
                if(!selectInstance(context, program, pointer, bound.pointer(), bound.size(),
                                   kNullLocation, false, found)) {
                    continue;
                }

                offer(functionSymbol(base, member), signature.result.type, klass.exported, StringId());
            }
        }
    });
}

/*
 * The named fields of one record, as items. Shared by the two positions that offer fields - after a
 * `.`, and inside the braces of a construction - so that a field looks the same whichever asked.
 */
void collectFields(Collector& into, DeclPtr<Type> type, U16 first, U16 count, U8 rank) {
    auto base = into.resolver.decl();
    auto& record = *(RecordType*)base[type];

    for(U16 i = first; i < U16(first + count); i++) {
        auto field = record.field(base, i);
        if(field.anonymous || field.name == StringId()) continue;

        into.add(fieldSymbol(base, type, field.index), field.occurrence.type, StringId(), rank);
    }
}

/*
 * §8.1's third kind: what follows a `.` - the fields of the receiver, then every visible function a
 * dot-call could reach. The fields first and nearest, because a field is what the receiver *has* and
 * where both answer to one name the field is what the resolver selects.
 *
 * The fields are read off the record the occurrence manifests as - `fieldRecordOf`, the same answer a
 * projection reads - and a pointer is followed one step, as a projection follows one.
 */
void collectMembers(Collector& into, const Occurrence& receiver) {
    auto& resolver = into.resolver;
    auto base = resolver.decl();

    auto occurrence = receiver;
    if(occurrence.type && base[occurrence.type]->kind == Type::Pointer) {
        occurrence = owned(((PointerType*)base[occurrence.type])->to);
    }

    if(!occurrence.type) return;

    if(auto record = fieldRecordOf(resolver.context, resolver.program, occurrence)) {
        auto& declared = *(RecordType*)base[record];
        if(declared.layout == RecordLayout::Single) {
            collectFields(into, record, 0, U16(declared.fields.size()), RankLocal);
        }
    }

    collectDotCallables(into, occurrence.type);
}

// By rank, then by the name as written - the text rather than the id, which is a hash and would make
// the order change whenever an unrelated name did. An insertion sort, for a list bounded by what one
// module and its direct imports declare.
bool sortsBefore(Context& context, const CompletionItem& a, const CompletionItem& b) {
    if(a.rank != b.rank) return a.rank < b.rank;

    auto& left = context.find(a.symbol.name);
    auto& right = context.find(b.symbol.name);
    auto shared = left.textLength < right.textLength ? left.textLength : right.textLength;

    for(U32 i = 0; i < shared; i++) {
        if(left.text[i] != right.text[i]) return left.text[i] < right.text[i];
    }

    return left.textLength < right.textLength;
}

void finishCompletion(Resolver& resolver, CompletionRequest& request) {
    auto& items = request.items;

    for(U32 i = 1; i < items.size(); i++) {
        auto item = items[i];
        auto j = i;
        while(j > 0 && sortsBefore(resolver.context, item, items[j - 1])) {
            items[j] = items[j - 1];
            j--;
        }

        items[j] = item;
    }
}

} // namespace

/*
 * The captures.
 *
 * Each is reached in place of the resolution the sentinel interrupted, and the caller then answers
 * nothing - the path every unresolvable name already takes, which is the best-trodden one in the
 * resolver. Nothing has to stop the body: the capture is complete, and what the rest of the walk
 * reports is about text the author has not finished writing, in a compile nobody shows diagnostics for.
 */

void Resolver::captureCompletion(DeclPtr<Type> expected) {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.expected = expected;

    collectBindings(collector);
    collectVisible(collector);
    finishCompletion(*this, request);
}

void Resolver::captureMemberCompletion(const Occurrence& receiver) {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.receiver = receiver.type;
    request.member = true;

    collectMembers(collector, receiver);
    finishCompletion(*this, request);
}

/*
 * A field of an update path - `{v | ori|gin: p}`.
 *
 * What a `.` offers, one step at a time: an update path is field selection with a value on the end of
 * it. What differs is what choosing one types - half of a `field: value` pair rather than a projection -
 * which is what `construct` says to the server.
 */
void Resolver::captureUpdateCompletion(const Occurrence& receiver) {
    if(!wantsCompletion(context)) return;

    captureMemberCompletion(receiver);
    context.completion->construct = true;
}

/*
 * §8.1's fourth kind - the fields of what is being written.
 *
 * Not a special case of the third: after a `.` the receiver is finished and its fields are the only
 * thing that can follow, while inside `Square {si|` the author has not committed to writing a field
 * name at all. So the fields lead and the scope follows, and only a cursor in front of a `:`
 * (`namesOnly`) drops the second half.
 */
void Resolver::captureConstructionCompletion(DeclPtr<Type> record, U16 first, U16 count,
                                             bool namesOnly) {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.constructed = record;
    request.construct = true;

    collectFields(collector, record, first, count, RankExpected);

    if(!namesOnly) {
        collectBindings(collector);
        collectVisible(collector);
    }

    finishCompletion(*this, request);
}

/*
 * An argument of a call - `f(x|` and `f(mo|: 1)`.
 *
 * The parameter *names* of every candidate the call could still reach, ranked first, and then what a
 * name in expression position offers. Both, because the position has not said which it is: an argument
 * may be written positionally, and the name that would make it a named one is what an author who cannot
 * remember the order is reaching for. Every candidate rather than one, because which one serves the
 * call is decided by arguments that are, by definition, not written yet.
 */
void Resolver::captureArgumentCompletion(const OverloadSet& set, DeclPtr<Type> expected, bool namesOnly) {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.expected = expected;

    for(Size c = 0; c < set.size(); c++) {
        auto pointer = set.at(c);
        auto& candidate = *decl()[pointer];

        for(U32 i = 0; i < U32(candidate.args.size()); i++) {
            if(!candidate.writesArg(i)) continue;

            auto& argument = *decl()[candidate.args.get(decl(), i)];
            if(argument.name == StringId()) continue;

            // The parameter as its own declaration records it, so hovering the item and hovering the
            // parameter describe one thing.
            Symbol symbol;
            symbol.kind = Symbol::Kind::Arg;
            symbol.module = candidate.module;
            symbol.function = pointer;
            symbol.name = argument.name;
            symbol.definition = argument.source;

            collector.add(symbol, argument.occurrence.type, StringId(), RankExpected, true);
        }
    }

    if(!namesOnly) {
        collectBindings(collector);
        collectVisible(collector);
    }

    finishCompletion(*this, request);
}

/*
 * §8.1's fifth kind - the constructors a pattern could name, and nothing else: a pattern position holds
 * a constructor, a literal or a name being introduced, and offering a function there would complete to
 * something that cannot be written. The pivot is the expected type, so the constructors of the value
 * being matched rank first.
 */
void Resolver::capturePatternCompletion(DeclPtr<Type> pivot) {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.expected = pivot;
    request.pattern = true;

    collectVisible(collector, Offered::Constructors);
    finishCompletion(*this, request);
}

/*
 * A type position - `x :: I|`, `fn f(p: Po|`, `-> |`.
 *
 * Types and aliases, and nothing a value could be: a function offered after `::` completes to a
 * program that does not parse. There is no expected type to rank by, so the ranking is where the
 * name is declared.
 */
void Resolver::captureTypeCompletion() {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);

    collectVisible(collector, Offered::Types);

    // And the types no declaration states - `I32`, `U8`, `F64` - which a position spelling a type
    // is where they are written most. `primitive.def`'s roster, answered through `findBuiltin` so
    // the item is the type the name resolves to.
    for(auto& primitive: intrinsic::primitiveTable) {
        auto name = context.addUnqualifiedName(primitive.name.ptr, U32(primitive.name.length));
        auto type = program.types.findBuiltin(context, name);
        if(!type) continue;

        Symbol symbol = typeSymbol(decl(), type);
        symbol.name = name;
        collector.add(symbol, nullptr, StringId(), U8(RankImported));
    }

    finishCompletion(*this, request);
}

/*
 * The name between backticks - `` a `sh| b ``.
 *
 * What `findOperator` would accept there, and nothing else: a function with a fixity declared for its
 * name somewhere in scope. The fixity is the filter because it is the rule - a two-argument function
 * with no `infixl` is refused between backticks however well its arguments fit - and because it is
 * what makes the list short: Core's bitwise operators rather than every function in the program.
 * A name reached only through a qualified import is left out, since a backtick holds a bare name, and
 * so is a symbolic one - `+` has a fixity too, and is written without the backticks.
 */
void Resolver::captureInfixCompletion() {
    if(!wantsCompletion(context)) return;

    auto& request = *context.completion;
    Collector collector { *this, request };
    begin(*this, request, collector);
    request.infix = true;

    collectVisible(collector);

    auto& items = request.items;
    U32 kept = 0;
    for(U32 i = 0; i < items.size(); i++) {
        auto& item = items[i];
        auto kind = item.symbol.kind;
        if(kind != Symbol::Kind::Function && kind != Symbol::Kind::ClassFun) continue;
        if(item.qualifier || operatorNamed(context, item.symbol.name)) continue;
        if(!findFixity(context, program, here(), item.symbol.name)) continue;

        items[kept++] = item;
    }

    items.resize(kept);
    finishCompletion(*this, request);
}

bool Resolver::captureArguments(ast::ParseList<ast::TupArg> written, const OverloadSet& set,
                                U32 leading) {
    if(!wantsCompletion(context)) return false;

    U32 index = 0;
    for(auto argument: written.contents(parse)) {
        auto at = index++;

        if(argument.name != StringId() && isCursorSentinel(context, argument.name)) {
            captureArgumentCompletion(set, nullptr, true);
            return true;
        }

        auto value = &argument.value;
        while(value->kind == ast::Expr::Nested) value = parse[value->nested];

        if(argument.name != StringId() || value->kind != ast::Expr::Var) continue;
        if(!isCursorSentinel(context, value->var)) continue;

        // The type the position asks for, where one candidate is the whole set and the argument is
        // written by position.
        DeclPtr<Type> expected = nullptr;
        if(auto sole = set.sole()) {
            auto& callee = *decl()[sole];
            auto position = firstWritten(callee) + leading + at;
            if(callee.continuationArg != Function::kNoContinuation && position >= callee.continuationArg) {
                position++;
            }

            if(position < callee.args.size()) {
                expected = decl()[callee.args.get(decl(), position)]->occurrence.type;
            }
        }

        captureArgumentCompletion(set, expected, false);
        return true;
    }

    return false;
}

bool Resolver::captureFields(ast::ParseList<ast::TupArg> written, DeclPtr<Type> record, U16 first,
                             U16 count) {
    if(!wantsCompletion(context)) return false;

    for(auto argument: written.contents(parse)) {
        if(argument.name != StringId() && isCursorSentinel(context, argument.name)) {
            captureConstructionCompletion(record, first, count, true);
            return true;
        }

        if(argument.name == StringId() && argument.value.kind == ast::Expr::Var &&
           isCursorSentinel(context, argument.value.var)) {
            captureConstructionCompletion(record, first, count, false);
            return true;
        }
    }

    return false;
}

} // namespace mir
