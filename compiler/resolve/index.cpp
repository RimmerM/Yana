#include "index.h"

/*
 * Storing.
 */

void SemanticIndex::addReference(const Reference& reference) {
    if(reference.source == kNullLocation) return;

    /*
     * The declaration comes along with the use.
     *
     * A local, an argument and a capture have no declaration walk to be recorded by - they are
     * introduced by a pattern in the middle of a body - so this is what makes a cursor sitting on
     * `let &total` find the same symbol a cursor on a use of it finds. It is not a duplicate of the
     * module-level walk either: `addDefinition` keeps the first answer for a location, and the walk
     * runs first.
     */
    if(reference.target.definition != kNullLocation &&
       !definitionByLocation.get(reference.target.definition)) {
        addDefinition(reference.target);
    }

    auto found = referenceByLocation.add(reference.source);
    if(found.existed) {
        references[*found.value] = reference;
        return;
    }

    *found.value = U32(references.size());
    references.push(reference);
}

void SemanticIndex::addDefinition(const Symbol& symbol) {
    if(symbol.definition == kNullLocation) return;

    auto found = definitionByLocation.add(symbol.definition);
    if(found.existed) {
        definitions[*found.value] = symbol;
        return;
    }

    *found.value = U32(definitions.size());
    definitions.push(symbol);
}

const Reference* SemanticIndex::findReference(LocationId source) const {
    if(source == kNullLocation) return nullptr;

    auto found = referenceByLocation.get(source);
    return found ? &references[found.unwrap()] : nullptr;
}

const Symbol* SemanticIndex::findDefinition(LocationId source) const {
    if(source == kNullLocation) return nullptr;

    auto found = definitionByLocation.get(source);
    return found ? &definitions[found.unwrap()] : nullptr;
}

bool SemanticIndex::same(const Symbol& a, const Symbol& b) {
    if(a.kind != b.kind) return false;

    switch(a.kind) {
        case Symbol::Kind::Local:
        case Symbol::Kind::Arg:
        case Symbol::Kind::Capture:
            // A slot index means nothing outside the function that has it, and a name means nothing
            // outside the scope that bound it - so the declaration is the identity here. Two `x` in
            // two `let`s are two symbols even though both are local 0 of one function.
            return a.function == b.function && a.definition == b.definition;
        default:
            return a.payload == b.payload && a.index == b.index && a.module == b.module;
    }
}

void SemanticIndex::findOccurrences(const Symbol& symbol, Array<const Reference*>& into) const {
    for(auto& reference: references) {
        if(same(reference.target, symbol)) into.push(&reference);
    }
}

/*
 * Recording. Null index, null location, or a symbol that resolved to nothing all mean the same
 * thing here: there is no answer worth keeping.
 */

void recordReference(Context& context, LocationId source, const Symbol& target, TypePtr type,
                     ModulePtr<ClassInstance> instance) {
    if(!context.index || source == kNullLocation) return;

    Reference reference;
    reference.source = source;
    reference.target = target;
    reference.type = type;
    reference.instance = instance;

    context.index->addReference(reference);
}

void recordDefinition(Context& context, const Symbol& symbol) {
    if(!context.index || symbol.definition == kNullLocation) return;
    context.index->addDefinition(symbol);
}

/*
 * The builders.
 */

Symbol functionSymbol(Module& module, ModulePtr<Function> pointer) {
    auto local = *module.arena;
    auto function = local[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Function;
    symbol.module = function->module;
    symbol.payload = pointer;
    symbol.name = function->name;
    symbol.definition = function->source;
    return symbol;
}

Symbol globalSymbol(Module& module, ModulePtr<Global> pointer) {
    auto local = *module.arena;
    auto global_ = local[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Global;
    symbol.module = global_->module;
    symbol.payload = pointer;
    symbol.name = global_->name;
    symbol.definition = global_->source;
    return symbol;
}

Symbol typeSymbol(Module& module, TypePtr type, StringId name) {
    auto global = *module.types;

    Symbol symbol;
    symbol.kind = Symbol::Kind::Type;
    symbol.module = &module;
    symbol.payload = type;
    symbol.name = name;

    if(type && global[type]->kind == Type::Record) {
        // An instantiation is not a declaration - `Maybe(Int)` is made from `Maybe`, and that is
        // what a jump has to land on. base() answers the declaration for both.
        auto record = (RecordType*)global[type];
        auto base = record->base(global);

        symbol.payload = (Type*)global[base] - global;
        symbol.name = global[base]->name;
        symbol.definition = global[base]->source;
    }

    return symbol;
}

Symbol aliasSymbol(Module& module, const TypeAlias& alias) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Alias;
    symbol.module = alias.module ? alias.module : &module;
    symbol.name = alias.name;
    symbol.definition = alias.source;
    return symbol;
}

Symbol constructorSymbol(Module& module, ConstructorRef reference) {
    auto global = *module.types;
    auto record = global[reference.record];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Constructor;
    symbol.module = &module;
    symbol.payload = reference.record;
    symbol.index = reference.index;

    if(reference.index < record->constructors.size()) {
        auto constructor = record->constructors.get(global, reference.index);
        symbol.name = constructor.name;
        symbol.definition = constructor.source;
    }

    // A constructor with no source of its own belongs to a declaration that has one - which is
    // every constructor Core generates rather than parses.
    if(symbol.definition == kNullLocation) symbol.definition = record->source;
    return symbol;
}

Symbol classSymbol(Module& module, GlobalPtr<TypeClass> pointer) {
    auto global = *module.types;
    auto typeClass = global[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Class;
    symbol.module = typeClass->module ? typeClass->module : &module;
    symbol.payload = pointer;
    symbol.name = typeClass->name;
    symbol.definition = typeClass->source;
    return symbol;
}

Symbol classFunSymbol(Module& module, GlobalPtr<TypeClass> pointer, U16 index) {
    auto global = *module.types;
    auto typeClass = global[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::ClassFun;
    symbol.module = typeClass->module ? typeClass->module : &module;
    symbol.payload = pointer;
    symbol.index = index;
    symbol.definition = typeClass->source;

    if(index < typeClass->functions.size()) {
        auto entry = typeClass->functions.get(global, index);
        symbol.name = entry.name;

        // The signature's own line, which is inside the class body rather than at its head.
        if(entry.fun) {
            auto signature = (*module.arena)[entry.fun];
            if(signature->source != kNullLocation) symbol.definition = signature->source;
        }
    }

    return symbol;
}

Symbol fieldSymbol(Module& module, TypePtr owner, U16 index, StringId name, LocationId definition) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Field;
    symbol.module = &module;
    symbol.payload = owner;
    symbol.index = index;
    symbol.name = name;
    symbol.definition = definition;
    return symbol;
}

Symbol typeVarSymbol(Module& module, GlobalPtr<GenType> pointer) {
    auto global = *module.types;
    auto variable = global[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::TypeVar;
    symbol.module = &module;
    symbol.payload = variable->env;
    symbol.index = variable->index;
    symbol.name = variable->name;
    symbol.definition = variable->source;
    return symbol;
}

Symbol moduleSymbol(Module& target) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Module;
    symbol.module = &target;
    symbol.name = target.name;
    return symbol;
}

/*
 * Describing.
 */

StringView symbolKindName(Symbol::Kind kind) {
    switch(kind) {
        case Symbol::Kind::Function: return "function"_v;
        case Symbol::Kind::Global: return "global"_v;
        case Symbol::Kind::Type: return "type"_v;
        case Symbol::Kind::Alias: return "alias"_v;
        case Symbol::Kind::Constructor: return "constructor"_v;
        case Symbol::Kind::Class: return "class"_v;
        case Symbol::Kind::ClassFun: return "class function"_v;
        case Symbol::Kind::Local: return "local"_v;
        case Symbol::Kind::Arg: return "argument"_v;
        case Symbol::Kind::Capture: return "capture"_v;
        case Symbol::Kind::Field: return "field"_v;
        case Symbol::Kind::TypeVar: return "type variable"_v;
        case Symbol::Kind::Module: return "module"_v;
        case Symbol::Kind::Import: return "import"_v;
    }

    return "symbol"_v;
}

/*
 * A parameter's default, written the way its declaration wrote it - see Arg::defaultBits.
 *
 * The bits are what the storage holds, so a payload-free record's constant is a constructor index
 * and printing it back as the constructor's name is what keeps `= False` readable as `False`. The
 * signed case is the same one `printValue` states: constants are stored sign-extended, so a
 * negative default is a very large unsigned number in the payload.
 *
 * Only the forms `evaluateConstant` accepts arrive here, which is what keeps this three cases rather
 * than a formatter.
 */
static void describeDefault(Context& context, GlobalBase global, TypePtr type, U64 bits,
                            StringBuilder& into) {
    auto declared = global[type];

    if(declared->kind == Type::Record && bits < ((RecordType*)declared)->constructors.size()) {
        into << context.findName(((RecordType*)declared)->constructors.get(global, Size(bits)).name);
        return;
    }

    if(isFloat(global, type)) {
        show(floatFromBits(global, type, bits), into);
        return;
    }

    if(declared->kind == Type::Int && ((IntType*)declared)->isSigned) {
        show(I64(bits), into);
        return;
    }

    show(bits, into);
}

// `fn name(a: T, b: U = 1) -> V`, from the resolved signature rather than from the source text. What
// the compiler decided is the point: a parameter whose type was inferred prints as what it became.
//
// `parameters` collects where each one landed, for signature help. Recorded here rather than
// re-derived, because a range into a string is only meaningful to whoever wrote the string.
static void describeFunction(Context& context, Module& module, Function& function, StringBuilder& into,
                             Array<SignatureParameter>* parameters = nullptr) {
    auto global = *module.types;
    auto local = *module.arena;

    into << (function.signature ? "class fn " : "fn ");
    into << context.findName(function.name);
    into << "(";

    auto first = true;
    for(auto pointer: function.args.contents(local)) {
        auto arg = local[pointer];
        if(!first) into << ", ";
        first = false;

        auto start = U32(into.size());

        if(arg->convention == ast::BindType::Ref) into << "&";
        else if(arg->convention == ast::BindType::Sink) into << "->";

        if(arg->name) {
            into << context.findName(arg->name);
            into << ": ";
        }

        describeType(context, global, arg->declaredType(), into);

        // Inside the parameter's own range, because it is part of what that parameter is: a reader
        // deciding whether to write the argument at all needs to see that it may be left out.
        if(arg->hasDefault()) {
            into << " = ";
            describeDefault(context, global, arg->declaredType(), arg->defaultBits.unwrap(), into);
        }

        if(parameters) parameters->push(SignatureParameter { start, U32(into.size()), arg->name });
    }

    into << ") -> ";
    describeType(context, global, function.returnType, into);
}

void describeSymbol(Context& context, const Symbol& symbol, TypePtr type, StringBuilder& into,
                    Array<SignatureParameter>* parameters) {
    auto module = symbol.module;

    switch(symbol.kind) {
        case Symbol::Kind::Function: {
            if(!module) break;
            describeFunction(context, *module, *(*module->arena)[ModulePtr<Function>(symbol.payload)], into,
                             parameters);
            return;
        }
        case Symbol::Kind::ClassFun: {
            if(!module) break;

            auto global = *module->types;
            auto typeClass = global[GlobalPtr<TypeClass>(symbol.payload)];
            if(symbol.index >= typeClass->functions.size()) break;

            auto entry = typeClass->functions.get(global, symbol.index);
            if(!entry.fun) break;

            describeFunction(context, *module, *(*module->arena)[entry.fun], into, parameters);
            into << " -- declared by class ";
            into << context.findName(typeClass->name);
            return;
        }
        case Symbol::Kind::Global: {
            if(!module) break;

            auto global_ = (*module->arena)[ModulePtr<Global>(symbol.payload)];
            into << (global_->mut ? "let &" : "let ");
            into << context.findName(global_->name);
            into << ": ";
            describeType(context, *module->types, global_->type, into);
            return;
        }
        case Symbol::Kind::Type:
        case Symbol::Kind::Alias: {
            into << (symbol.kind == Symbol::Kind::Alias ? "alias " : "data ");

            // A type with no declaration - a scalar, a tuple - prints as itself; there is no
            // `data` line anywhere for it to be quoted from.
            if(symbol.name) into << context.findName(symbol.name);
            else if(module) describeType(context, *module->types, TypePtr(symbol.payload), into);

            // The occurrence's own type, which for a generic declaration is the instantiation this
            // one was - `Maybe(Int)` where the declaration is `Maybe(a)`.
            if(type && module) {
                into << " -- here: ";
                describeType(context, *module->types, type, into);
            }

            return;
        }
        case Symbol::Kind::Constructor: {
            if(!module) break;

            auto global = *module->types;
            auto record = global[GlobalPtr<RecordType>(symbol.payload)];
            if(symbol.index >= record->constructors.size()) break;

            auto constructor = record->constructors.get(global, symbol.index);
            into << context.findName(record->name);
            into << ".";
            into << context.findName(constructor.name);

            if(constructor.content) {
                into << "(";

                /*
                 * A payload tuple is written out field by field rather than through describeType,
                 * so that each field's range can be recorded - the text is the same either way,
                 * and a range into it is only meaningful to whoever wrote the string.
                 *
                 * Constructing a record is a call with parameters exactly as a function is, which
                 * is what signature help needs: `Square {side: 3}` has an argument the caret is in
                 * just as `scale(s, 2)` does.
                 */
                auto content = global[constructor.content];
                if(parameters && content->kind == Type::Tup) {
                    auto tuple = (TupType*)content;
                    into << "{";

                    for(Size i = 0; i < tuple->fields.size(); i++) {
                        auto field = tuple->fields.get(global, i);
                        if(i) into << ", ";

                        auto start = U32(into.size());
                        if(field.name) into << context.findName(field.name) << ": ";
                        if(field.boxed) into << "@box ";
                        describeType(context, global, field.type, into);

                        parameters->push(SignatureParameter { start, U32(into.size()), field.name });
                    }

                    into << "}";
                } else {
                    auto start = U32(into.size());
                    describeType(context, global, constructor.content, into);

                    // A payload that is not a tuple is one argument, and it is the whole of what
                    // was written - so the parameter is the type itself.
                    if(parameters) parameters->push(SignatureParameter { start, U32(into.size()) });
                }

                into << ")";
            }

            return;
        }
        case Symbol::Kind::Class: {
            into << "class ";
            into << context.findName(symbol.name);
            return;
        }
        case Symbol::Kind::Module:
        case Symbol::Kind::Import: {
            into << "module ";
            into << context.findName(symbol.name);
            return;
        }
        default:
            break;
    }

    // Everything with no declaration of its own to print - the three function-local kinds, a field,
    // a type variable - is its kind, its name and whatever type the occurrence had.
    into << symbolKindName(symbol.kind);
    into << " ";
    into << context.findName(symbol.name);

    if(type && module) {
        into << ": ";
        describeType(context, *module->types, type, into);
    }
}
