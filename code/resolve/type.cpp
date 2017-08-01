#include "type.h"
#include "../parse/ast.h"
#include "module.h"

Type unitType{Type::Unit};
Type errorType{Type::Error};
Type stringType{Type::String};

FloatType floatTypes[FloatType::KindCount] = {
    {16, FloatType::F16},
    {32, FloatType::F32},
    {64, FloatType::F64}
};

IntType intTypes[IntType::KindCount] = {
    {1, IntType::Bool},
    {32, IntType::Int},
    {64, IntType::Long}
};

static Type* findType(Module* module, ast::Type* type) {
    switch(type->kind) {
        case ast::Type::Error:
            return &errorType;
        case ast::Type::Unit:
            return &unitType;
        case ast::Type::Ptr: {
            auto ast = (ast::PtrType*)type;
            auto content = resolveType(module, ast->type);
            return new (module) PtrType(content);
        }
        case ast::Type::Tup:
            break;
        case ast::Type::Gen:
            break;
        case ast::Type::App:
            break;
        case ast::Type::Con:
            break;
        case ast::Type::Fun: {
            auto ast = (ast::FunType*)type;
            auto ret = resolveType(module, ast->ret);
            U32 argc = 0;
            auto arg = ast->args;
            while(arg) {
                argc++;
                arg = arg->next;
            }

            FunArg* args = nullptr;
            if(argc > 0) {
                args = (FunArg*)module->memory.alloc(sizeof(FunArg) * argc);
                arg = ast->args;
                for(U32 i = 0; i < argc; i++) {
                    args[i].type = resolveType(module, arg->item.type);
                    args[i].index = i;
                    args[i].name = arg->item.name;
                    arg = arg->next;
                }
            }

            auto fun = new (module->memory) FunType();
            fun->args = args;
            fun->result = ret;
            fun->argCount = argc;
            return fun;
        }
        case ast::Type::Arr: {
            auto ast = (ast::ArrType*)type;
            auto content = resolveType(module, ast->type);
            return new (module->memory) ArrayType(content);
        }
        case ast::Type::Map:{
            auto ast = (ast::MapType*)type;
            auto from = resolveType(module, ast->from);
            auto to = resolveType(module, ast->to);
            return new (module->memory) MapType(from, to);
        }
    }
}

Type* resolveDefinition(Module* module, Type* type) {

}

Type* resolveType(Module* module, ast::Type* type) {

}