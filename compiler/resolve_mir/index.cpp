#include "index.h"
#include "print.h"

namespace mir {

/*
 * Storing.
 */

void SemanticIndex::addReference(const Reference& reference) {
    if(reference.source == kNullLocation) return;

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
            // A slot means nothing outside the function that has it, and a name nothing outside the
            // scope that bound it - so the declaration is the identity. Two `x` in two `let`s are
            // two symbols even where one body reuses nothing.
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

void recordReference(Context& context, LocationId source, const Symbol& target, DeclPtr<Type> type,
                     DeclPtr<Instance> instance) {
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

Symbol functionSymbol(DeclBase base, DeclPtr<Function> pointer) {
    auto& function = *base[pointer];

    Symbol symbol;
    symbol.kind = function.klass ? Symbol::Kind::ClassFun : Symbol::Kind::Function;
    symbol.module = function.module;
    symbol.payload = pointer.offset;
    symbol.name = function.name;
    symbol.definition = function.source;
    return symbol;
}

Symbol globalSymbol(Program& program, DeclPtr<Global> pointer, DeclPtr<mir::Module> module) {
    auto base = *program.decl;
    auto& global = *base[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Global;
    symbol.module = module;

    for(auto owner: program.modules) {
        if(symbol.module) break;

        for(auto candidate: base[owner]->globals.contents(base)) {
            if(candidate == pointer) symbol.module = owner;
        }
    }

    symbol.payload = pointer.offset;
    symbol.name = global.name;
    symbol.definition = global.source;
    return symbol;
}

Symbol typeSymbol(DeclBase base, DeclPtr<Type> type) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Type;
    symbol.payload = type.offset;

    if(type && base[type]->kind == Type::Record) {
        auto record = DeclPtr<RecordType>(type.offset);
        auto& written = *base[record];

        // An instantiation is not a declaration. A tuple is not one either, and is left naming
        // itself - it has no name and nowhere to jump to.
        if(written.instanceOf) record = written.instanceOf;

        auto& declared = *base[record];
        if(!declared.tuple) {
            symbol.payload = record.offset;
            symbol.module = declared.module;
            symbol.name = declared.name;
            symbol.definition = declared.source;
        }
    }

    return symbol;
}

Symbol aliasSymbol(DeclBase base, DeclPtr<Alias> pointer) {
    auto& alias = *base[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Alias;
    symbol.module = alias.module;
    symbol.payload = pointer.offset;
    symbol.name = alias.name;
    symbol.definition = alias.source;
    return symbol;
}

Symbol constructorSymbol(DeclBase base, ConstructorRef constructor) {
    auto record = constructor.record;
    if(record && base[record]->instanceOf) record = base[record]->instanceOf;

    Symbol symbol;
    symbol.kind = Symbol::Kind::Constructor;
    symbol.payload = record.offset;
    symbol.index = constructor.index;
    if(!record) return symbol;

    auto& declared = *base[record];
    symbol.module = declared.module;

    if(constructor.index < declared.cons.size()) {
        auto arm = declared.cons.get(base, constructor.index);
        symbol.name = arm.name;
        symbol.definition = arm.source;
    }

    // A constructor with no location of its own belongs to a declaration that has one.
    if(symbol.definition == kNullLocation) symbol.definition = declared.source;
    return symbol;
}

Symbol classSymbol(DeclBase base, DeclPtr<Class> pointer) {
    auto& klass = *base[pointer];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Class;
    symbol.module = klass.module;
    symbol.payload = pointer.offset;
    symbol.name = klass.name;
    symbol.definition = klass.source;
    return symbol;
}

Symbol fieldSymbol(DeclBase base, DeclPtr<Type> type, U16 field) {
    auto record = DeclPtr<RecordType>(type.offset);
    if(base[record]->instanceOf) record = base[record]->instanceOf;

    auto& declared = *base[record];

    Symbol symbol;
    symbol.kind = Symbol::Kind::Field;
    symbol.module = declared.module;
    symbol.payload = record.offset;
    symbol.index = field;

    // At the record rather than at the field: what a field records as its source is where its *type*
    // was written, which is what a diagnostic about the type points at and not what a jump to the
    // field should land on.
    if(field < declared.fields.size()) symbol.name = declared.field(base, field).name;
    symbol.definition = declared.source;
    return symbol;
}

Symbol typeVarSymbol(DeclBase base, DeclPtr<mir::Module> module, DeclPtr<Type> variable) {
    auto& declared = *(VariableType*)base[variable];

    Symbol symbol;
    symbol.kind = Symbol::Kind::TypeVar;
    symbol.module = module;
    symbol.payload = variable.offset;
    symbol.name = declared.name;
    symbol.definition = declared.source;
    return symbol;
}

Symbol moduleSymbol(DeclBase base, DeclPtr<mir::Module> module) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Module;
    symbol.module = module;
    symbol.name = base[module]->name;
    return symbol;
}

Symbol localSymbol(DeclBase base, MirBase mir, DeclPtr<Function> function, Local& local) {
    Symbol symbol;
    symbol.kind = local.isParameter() ? Symbol::Kind::Arg : Symbol::Kind::Local;
    symbol.module = base[function]->module;
    symbol.function = function;
    symbol.payload = (&local - mir).offset;
    symbol.name = local.name;
    symbol.definition = local.source;
    return symbol;
}

Local* symbolLocal(Program& program, const Symbol& symbol) {
    if(symbol.kind != Symbol::Kind::Local && symbol.kind != Symbol::Kind::Arg) return nullptr;
    if(!symbol.payload) return nullptr;

    return (*program.mir)[MirPtr<Local>(symbol.payload)];
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
    }

    return "symbol"_v;
}

bool isCompilerName(Context& context, StringId name) {
    auto& identifier = context.find(name);

    for(U32 i = 0; i < identifier.textLength; i++) {
        if(identifier.text[i] == '$') return true;
    }

    return false;
}

String constantText(Context& context, DeclBase base, const Constant& constant) {
    Net::Writer writer(128);
    printConstant(writer, context, base, constant);

    auto written = writer.getBuffered();
    return ownedString((const char*)written.ptr, written.length);
}

static void append(StringBuilder& into, const String& text) {
    into.append(text.text(), text.size());
}

// The instance whose member this is, or null. A walk, and one `describeFunction` takes only for a
// symbol that says it is one - see below.
static DeclPtr<Instance> owningInstance(Program& program, DeclPtr<Function> pointer) {
    auto base = *program.decl;

    for(auto module: program.modules) {
        for(auto instance: base[module]->instances.contents(base)) {
            for(auto member: base[instance]->members.contents(base)) {
                if(member == pointer) return instance;
            }
        }
    }

    return nullptr;
}

/*
 * `fn name(a: T, &b: U = 1) -> V`, from the resolved signature rather than from the source text - a
 * position whose type was inferred prints as what it became.
 *
 * Only the positions a call *writes*: a lens's continuation is the desugaring's and a lifted lambda's
 * environment is the closure's, and neither is something a caller could fill. A traversal's result is
 * what it hands over, which is what its declaration wrote after the arrow.
 */
static void describeFunction(Context& context, Program& program, const Symbol& symbol,
                             StringBuilder& into, Array<SignatureParameter>* parameters) {
    auto base = *program.decl;
    auto& function = *base[DeclPtr<Function>(symbol.payload)];

    if(function.klass) into << "class ";
    if(function.iterator) into << "iter ";
    else if(function.lens) into << "lens ";

    /*
     * An instance member is recorded under the name it was written with - `resolve_class.cpp` - and
     * is described under it. A symbol's name differs from its function's for one other reason, a
     * dot-call completion labelling `Point.span` as `span`, and that one still describes the function
     * by its own name - so the instance is what decides, and it is looked up only where the names
     * already differ.
     */
    auto name = function.name;
    DeclPtr<Instance> instance = nullptr;

    if(!function.klass && symbol.name && symbol.name != function.name) {
        instance = owningInstance(program, DeclPtr<Function>(symbol.payload));
        if(instance) name = symbol.name;
    }

    into << "fn " << context.findName(name) << "(";

    auto first = true;
    for(U32 i = 0; i < U32(function.args.size()); i++) {
        if(!function.writesArg(i)) continue;

        auto& arg = *base[function.args.get(base, i)];
        if(!first) into << ", ";
        first = false;

        auto start = U32(into.size());
        if(arg.lazy) into << "@lazy ";
        into << conventionMark(arg.occurrence.contract, Access::Shared);

        if(arg.name) into << context.findName(arg.name) << ": ";
        append(into, bindingText(context, program, arg.occurrence));

        // Inside the parameter's own range: a reader deciding whether to write the argument at all
        // needs to see that it may be left out.
        if(arg.def.isSet()) {
            into << " = ";
            append(into, constantText(context, base, arg.def));
        }

        if(parameters) parameters->push(SignatureParameter { start, U32(into.size()), arg.name });
    }

    into << ") -> ";

    // The mark, unless the spelling already carries it: a borrow through a published origin is
    // `'T`, and `bindingText` writes that tick itself.
    auto& result = function.lens ? function.handover : function.result;
    auto ticked = result.contract.origin() == Origin::Slot && result.contract.access() == Access::Shared;
    if(!ticked) into << conventionMark(result.contract, function.lens ? Access::Shared : Access::Owned);
    append(into, bindingText(context, program, result));

    if(function.klass) {
        into << " -- declared by class " << context.findName(base[function.klass]->name);
    } else if(instance) {
        auto& owner = *base[instance];
        into << " -- in instance " << context.findName(base[owner.klass]->name) << "(";

        auto first = true;
        for(auto type: owner.args.contents(base)) {
            if(!first) into << ", ";
            first = false;
            append(into, typeText(context, base, type));
        }

        into << ")";
    }
}

/*
 * `Shape.Circle({radius: I32})` - a constructor as the call it is.
 *
 * Constructing a record is a call with parameters exactly as a function is, which is what signature
 * help reads: `Square {side: 3}` has an argument the caret is in just as `scale(s, 2)` does. A payload
 * the declaration did not name is one positional argument, and a constructor that carries nothing is
 * finished by its name.
 */
static void describeConstructor(Context& context, Program& program, RecordType& record, U16 index,
                                StringBuilder& into, Array<SignatureParameter>* parameters) {
    auto base = *program.decl;
    if(index >= record.cons.size()) return;

    auto arm = record.cons.get(base, index);
    into << context.findName(record.name) << "." << context.findName(arm.name);
    if(!arm.count) return;

    into << "(";

    auto payload = arm.count == 1 && record.field(base, arm.first).anonymous;
    if(!payload) into << "{";

    for(U16 i = 0; i < arm.count; i++) {
        auto field = record.field(base, U16(arm.first + i));
        if(i) into << ", ";

        auto start = U32(into.size());
        if(!payload && field.name) into << context.findName(field.name) << ": ";
        if(field.boxed) into << "@box ";
        append(into, typeText(context, base, field.occurrence.type));

        if(parameters) {
            parameters->push(SignatureParameter { start, U32(into.size()),
                                                  payload ? StringId() : field.name });
        }
    }

    if(!payload) into << "}";
    into << ")";
}

void describeSymbol(Context& context, Program& program, const Symbol& symbol, DeclPtr<Type> type,
                    StringBuilder& into, Array<SignatureParameter>* parameters) {
    auto base = *program.decl;

    switch(symbol.kind) {
        case Symbol::Kind::Function:
        case Symbol::Kind::ClassFun:
            describeFunction(context, program, symbol, into, parameters);
            return;
        case Symbol::Kind::Global: {
            auto& global = *base[DeclPtr<Global>(symbol.payload)];
            into << (global.writable ? "let &" : "let ") << context.findName(global.name) << ": ";
            append(into, typeText(context, base, global.occurrence.type));
            return;
        }
        case Symbol::Kind::Type:
        case Symbol::Kind::Alias: {
            into << (symbol.kind == Symbol::Kind::Alias ? "alias " : "data ");

            // A type with no declaration - a width, a tuple - prints as itself, since there is no
            // `data` line anywhere for it to be quoted from.
            if(symbol.name) into << context.findName(symbol.name);
            else append(into, typeText(context, base, DeclPtr<Type>(symbol.payload)));

            // The occurrence's own type, which for a generic declaration is the instantiation this
            // one was - `Maybe(I32)` where the declaration is `Maybe(a)`.
            if(type) {
                into << " -- here: ";
                append(into, typeText(context, base, type));
            }

            return;
        }
        case Symbol::Kind::Constructor:
            describeConstructor(context, program, *base[DeclPtr<RecordType>(symbol.payload)],
                                symbol.index, into, parameters);
            return;
        case Symbol::Kind::Class:
            into << "class " << context.findName(symbol.name);
            return;
        case Symbol::Kind::Module:
            into << "module " << context.findName(symbol.name);
            return;
        default:
            break;
    }

    // Everything with no declaration of its own to print - the function-local kinds, a field, a type
    // variable - is its kind, its name and whatever type the occurrence had.
    into << symbolKindName(symbol.kind) << " " << context.findName(symbol.name);

    if(type) {
        into << ": ";
        append(into, typeText(context, base, type));
    }
}

} // namespace mir
