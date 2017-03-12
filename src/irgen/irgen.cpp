#include <unordered_map>
#include <vector>
#include <cassert>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include "irgen.h"
#include "mangle.h"
#include "../sema/typecheck.h"
#include "../parser/parser.hpp"
#include "../driver/utility.h"

using namespace delta;

namespace {

llvm::Value* codegen(const Expr& expr);
llvm::Value* codegenLvalue(const Expr& expr);
void deferDeinitCallOf(llvm::Value* value);
void createDeinitCall(llvm::Value* valueToDeinit);

struct Scope {
    llvm::SmallVector<const Expr*, 8> deferredExprs;
    llvm::SmallVector<llvm::Value*, 8> valuesToDeinit;

    void onScopeEnd() {
        for (const Expr* expr : llvm::reverse(deferredExprs)) codegen(*expr);
        for (llvm::Value* value : llvm::reverse(valuesToDeinit)) createDeinitCall(value);
    }

    void clear() {
        deferredExprs.clear();
        valuesToDeinit.clear();
    }
};

class GenericFuncInstantiation {
public:
    const GenericFuncDecl& decl;
    llvm::ArrayRef<Type> genericArgs;
    llvm::Function* func;
};

llvm::LLVMContext ctx;
llvm::IRBuilder<> builder(ctx);
llvm::Module module("", ctx);
std::unordered_map<std::string, llvm::Value*> globalValues;
std::unordered_map<std::string, llvm::Value*> localValues;
std::unordered_map<std::string, llvm::Function*> funcs;
std::unordered_map<std::string, std::pair<llvm::StructType*, const TypeDecl*>> structs;
std::unordered_map<std::string, llvm::Type*> currentGenericArgs;
std::vector<GenericFuncInstantiation> genericFuncInstantiations;
const std::vector<Decl>* globalDecls;
const Decl* currentDecl;
llvm::SmallVector<Scope, 4> scopes;
llvm::BasicBlock::iterator lastAlloca;

/// The basic blocks to branch to on a 'break' statement, one element per scope.
llvm::SmallVector<llvm::BasicBlock*, 4> breakTargets;

/// @param type The Delta type of the variable, or null if the variable is 'this'.
void setLocalValue(Type type, std::string name, llvm::Value* value) {
    bool wasInserted = localValues.emplace(std::move(name), value).second;
    (void) wasInserted;
    assert(wasInserted);

    if (type && type.isBasicType()) {
        llvm::Function* deinit = module.getFunction(mangleDeinitDecl(type.getName()));
        if (deinit) deferDeinitCallOf(value);
    }
}

void codegen(const Decl& decl);

llvm::Value* findValue(llvm::StringRef name) {
    auto it = localValues.find(name);
    if (it == localValues.end()) {
        it = globalValues.find(name);

        // FIXME: It would probably be better to not access the symbol table here.
        if (it == globalValues.end()) {
            codegen(findInSymbolTable(name, SrcLoc::invalid()));
            it = globalValues.find(name);
            assert(it != globalValues.end());
        }
    }
    return it->second;
}

template<typename From, typename To>
std::vector<To> map(const std::vector<From>& from, To (&func)(const From&)) {
    std::vector<To> to;
    to.reserve(from.size());
    for (const auto& e : from) to.emplace_back(func(e));
    return to;
}

void codegen(const TypeDecl& decl);

llvm::Type* toIR(Type type) {
    switch (type.getKind()) {
        case TypeKind::BasicType: {
            llvm::StringRef name = type.getName();
            if (name == "void") return llvm::Type::getVoidTy(ctx);
            if (name == "bool") return llvm::Type::getInt1Ty(ctx);
            if (name == "char") return llvm::Type::getInt8Ty(ctx);
            if (name == "int" || name == "uint") return llvm::Type::getInt32Ty(ctx);
            if (name == "int8" || name == "uint8") return llvm::Type::getInt8Ty(ctx);
            if (name == "int16" || name == "uint16") return llvm::Type::getInt16Ty(ctx);
            if (name == "int32" || name == "uint32") return llvm::Type::getInt32Ty(ctx);
            if (name == "int64" || name == "uint64") return llvm::Type::getInt64Ty(ctx);
            auto it = structs.find(name);
            if (it == structs.end()) {
                // Is it a generic parameter?
                auto genericArg = currentGenericArgs.find(name);
                if (genericArg != currentGenericArgs.end()) return genericArg->second;

                // Custom type that has not been declared yet, search for it in the symbol table.
                codegen(findInSymbolTable(name, SrcLoc::invalid()).getTypeDecl());
                it = structs.find(name);
            }
            return it->second.first;
        }
        case TypeKind::ArrayType:
            return llvm::ArrayType::get(toIR(type.getElementType()), type.getArraySize());
        case TypeKind::TupleType:
            llvm_unreachable("IRGen doesn't support tuple types yet");
        case TypeKind::FuncType:
            llvm_unreachable("IRGen doesn't support function types yet");
        case TypeKind::PtrType: {
            auto* pointeeType = toIR(type.getPointee());
            if (!pointeeType->isVoidTy()) return llvm::PointerType::get(pointeeType, 0);
            else return llvm::PointerType::get(llvm::Type::getInt8Ty(ctx), 0);
        }
    }
}

llvm::Value* codegen(const VariableExpr& expr) {
    auto* value = findValue(expr.identifier);
    if (auto* arg = llvm::dyn_cast<llvm::Argument>(value)) return arg;
    return builder.CreateLoad(value, expr.identifier);
}

llvm::Value* codegenLvalue(const VariableExpr& expr) {
    return findValue(expr.identifier);
}

llvm::Value* codegen(const StrLiteralExpr& expr, const Expr& parent) {
    if (parent.getType().getPointee().isArrayType()) {
        return builder.CreateGlobalString(expr.value);
    } else {
        // Passing as C-string, i.e. char pointer.
        return builder.CreateGlobalStringPtr(expr.value);
    }
}

llvm::Value* codegen(const IntLiteralExpr& expr, const Expr& parent) {
    return llvm::ConstantInt::getSigned(toIR(parent.getType()), expr.value);
}

llvm::Value* codegen(const BoolLiteralExpr& expr) {
    return expr.value ? llvm::ConstantInt::getTrue(ctx) : llvm::ConstantInt::getFalse(ctx);
}

llvm::Value* codegen(const NullLiteralExpr& expr, const Expr& parent) {
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(toIR(parent.getType())));
}

llvm::Value* codegen(const ArrayLiteralExpr& expr) {
    auto* arrayType = llvm::ArrayType::get(toIR(expr.elements[0]->getType()), expr.elements.size());
    std::vector<llvm::Constant*> values;
    values.reserve(expr.elements.size());
    for (auto& e : expr.elements)
        values.emplace_back(llvm::cast<llvm::Constant>(codegen(*e)));
    return llvm::ConstantArray::get(arrayType, values);
}

using UnaryCreate   = llvm::Value* (llvm::IRBuilder<>::*)(llvm::Value*, const llvm::Twine&, bool, bool);
using BinaryCreate0 = llvm::Value* (llvm::IRBuilder<>::*)(llvm::Value*, llvm::Value*, const llvm::Twine&);
using BinaryCreate1 = llvm::Value* (llvm::IRBuilder<>::*)(llvm::Value*, llvm::Value*, const llvm::Twine&, bool);
using BinaryCreate2 = llvm::Value* (llvm::IRBuilder<>::*)(llvm::Value*, llvm::Value*, const llvm::Twine&, bool, bool);

llvm::Value* codegenPrefixOp(const PrefixExpr& expr, UnaryCreate create) {
    return (builder.*create)(codegen(*expr.operand), "", false, false);
}

llvm::Value* codegenNot(const PrefixExpr& expr) {
    return builder.CreateNot(codegen(*expr.operand), "");
}

llvm::Value* codegen(const PrefixExpr& expr) {
    switch (expr.op.rawValue) {
        case PLUS:  return codegen(*expr.operand);
        case MINUS: return codegenPrefixOp(expr, &llvm::IRBuilder<>::CreateNeg);
        case STAR:  return builder.CreateLoad(codegen(*expr.operand));
        case AND:   return codegenLvalue(*expr.operand);
        case NOT:   return codegenNot(expr);
        case COMPL: return codegenNot(expr);
        default:    llvm_unreachable("invalid prefix operator");
    }
}

llvm::Value* codegenLvalue(const PrefixExpr& expr) {
    switch (expr.op.rawValue) {
        case STAR:  return codegen(*expr.operand);
        default:    llvm_unreachable("invalid lvalue prefix operator");
    }
}

llvm::Value* codegenBinaryOp(llvm::Value* lhs, llvm::Value* rhs, BinaryCreate0 create) {
    return (builder.*create)(lhs, rhs, "");
}

llvm::Value* codegenBinaryOp(llvm::Value* lhs, llvm::Value* rhs, BinaryCreate1 create) {
    return (builder.*create)(lhs, rhs, "", false);
}

llvm::Value* codegenBinaryOp(llvm::Value* lhs, llvm::Value* rhs, BinaryCreate2 create) {
    return (builder.*create)(lhs, rhs, "", false, false);
}

llvm::Value* codegenLogicalAnd(const Expr& left, const Expr& right) {
    auto* lhsBlock = builder.GetInsertBlock();
    auto* rhsBlock = llvm::BasicBlock::Create(ctx, "andRHS", builder.GetInsertBlock()->getParent());
    auto* endBlock = llvm::BasicBlock::Create(ctx, "andEnd", builder.GetInsertBlock()->getParent());

    llvm::Value* lhs = codegen(left);
    builder.CreateCondBr(lhs, rhsBlock, endBlock);

    builder.SetInsertPoint(rhsBlock);
    llvm::Value* rhs = codegen(right);
    builder.CreateBr(endBlock);

    builder.SetInsertPoint(endBlock);
    llvm::PHINode* phi = builder.CreatePHI(llvm::IntegerType::getInt1Ty(ctx), 2, "and");
    phi->addIncoming(lhs, lhsBlock);
    phi->addIncoming(rhs, rhsBlock);
    return phi;
}

llvm::Value* codegenLogicalOr(const Expr& left, const Expr& right) {
    auto* lhsBlock = builder.GetInsertBlock();
    auto* rhsBlock = llvm::BasicBlock::Create(ctx, "orRHS", builder.GetInsertBlock()->getParent());
    auto* endBlock = llvm::BasicBlock::Create(ctx, "orEnd", builder.GetInsertBlock()->getParent());

    llvm::Value* lhs = codegen(left);
    builder.CreateCondBr(lhs, endBlock, rhsBlock);

    builder.SetInsertPoint(rhsBlock);
    llvm::Value* rhs = codegen(right);
    builder.CreateBr(endBlock);

    builder.SetInsertPoint(endBlock);
    llvm::PHINode* phi = builder.CreatePHI(llvm::IntegerType::getInt1Ty(ctx), 2, "or");
    phi->addIncoming(lhs, lhsBlock);
    phi->addIncoming(rhs, rhsBlock);
    return phi;
}

llvm::Value* codegenBinaryOp(BinaryOperator op, llvm::Value* lhs, llvm::Value* rhs, const Expr& leftExpr) {
    switch (op.rawValue) {
        case EQ:    return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateICmpEQ);
        case NE:    return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateICmpNE);
        case LT:    return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           &llvm::IRBuilder<>::CreateICmpSLT :
                                           &llvm::IRBuilder<>::CreateICmpULT);
        case LE:    return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           &llvm::IRBuilder<>::CreateICmpSLE :
                                           &llvm::IRBuilder<>::CreateICmpULE);
        case GT:    return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           &llvm::IRBuilder<>::CreateICmpSGT :
                                           &llvm::IRBuilder<>::CreateICmpUGT);
        case GE:    return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           &llvm::IRBuilder<>::CreateICmpSGE :
                                           &llvm::IRBuilder<>::CreateICmpUGE);
        case PLUS:  return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateAdd);
        case MINUS: return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateSub);
        case STAR:  return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateMul);
        case SLASH: return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           &llvm::IRBuilder<>::CreateSDiv :
                                           &llvm::IRBuilder<>::CreateUDiv);
        case AND:   return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateAnd);
        case OR:    return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateOr);
        case XOR:   return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateXor);
        case LSHIFT:return codegenBinaryOp(lhs, rhs, &llvm::IRBuilder<>::CreateShl);
        case RSHIFT:return codegenBinaryOp(lhs, rhs, leftExpr.getType().isSigned() ?
                                           (BinaryCreate1) &llvm::IRBuilder<>::CreateAShr :
                                           (BinaryCreate1) &llvm::IRBuilder<>::CreateLShr);
        default:    llvm_unreachable("all cases handled");
    }
}

llvm::Value* codegenShortCircuitBinaryOp(BinaryOperator op, const Expr& lhs, const Expr& rhs) {
    switch (op.rawValue) {
        case AND_AND: return codegenLogicalAnd(lhs, rhs);
        case OR_OR:   return codegenLogicalOr(lhs, rhs);
        default:      llvm_unreachable("invalid short-circuit binary operator");
    }
}

llvm::Value* codegen(const BinaryExpr& expr) {
    assert(expr.left->getType().isImplicitlyConvertibleTo(expr.right->getType())
        || expr.right->getType().isImplicitlyConvertibleTo(expr.left->getType()));

    switch (expr.op.rawValue) {
        case AND_AND: case OR_OR:
            return codegenShortCircuitBinaryOp(expr.op, *expr.left, *expr.right);
        default:
            llvm::Value* lhs = codegen(*expr.left);
            llvm::Value* rhs = codegen(*expr.right);
            return codegenBinaryOp(expr.op, lhs, rhs, *expr.left);
    }
}

llvm::Function* getFuncForCall(const CallExpr&);

llvm::Value* codegenForPassing(const Expr& expr, llvm::Type* targetType = nullptr) {
    if (expr.isRvalue() || expr.isStrLiteralExpr() || expr.isArrayLiteralExpr()) return codegen(expr);
    Type exprType = expr.getType();
    if (exprType.isPtrType()) exprType = exprType.getPointee();

    auto it = structs.find(exprType.getName());
    if (it == structs.end() || it->second.second->passByValue()) {
        if (expr.getType().isPtrType() && !targetType->isPointerTy()) {
            return builder.CreateLoad(codegen(expr));
        }
    } else if (!expr.getType().isPtrType()) {
        return codegenLvalue(expr);
    }
    return codegen(expr);
}

llvm::Value* codegen(const CallExpr& expr) {
    llvm::Function* func;
    if (expr.isInitializerCall) {
        func = module.getFunction(mangleInitDecl(expr.funcName));
    } else {
        func = getFuncForCall(expr);
    }

    auto param = func->arg_begin();
    llvm::SmallVector<llvm::Value*, 16> args;
    if (expr.isMemberFuncCall()) args.emplace_back(codegenForPassing(*expr.receiver, param++->getType()));
    for (const auto& arg : expr.args) args.emplace_back(codegenForPassing(*arg.value, param++->getType()));

    return builder.CreateCall(func, args);
}

llvm::Value* codegen(const CastExpr& expr) {
    auto* value = codegen(*expr.expr);
    auto* type = toIR(expr.type);
    if (value->getType()->isIntegerTy() && type->isIntegerTy()) {
        return builder.CreateIntCast(value, type, expr.expr->getType().isSigned());
    }
    return builder.CreateBitOrPointerCast(value, type);
}

llvm::Value* codegenLvalue(const MemberExpr& expr) {
    auto* value = codegenLvalue(*expr.base);
    auto baseType = value->getType();
    if (baseType->isPointerTy()) {
        baseType = baseType->getPointerElementType();
        if (baseType->isPointerTy()) {
            baseType = baseType->getPointerElementType();
            value = builder.CreateLoad(value);
        }
        auto index = structs.find(baseType->getStructName())->second.second->getFieldIndex(expr.member);
        return builder.CreateStructGEP(nullptr, value, index);
    } else {
        auto index = structs.find(baseType->getStructName())->second.second->getFieldIndex(expr.member);
        return builder.CreateExtractValue(value, index);
    }
}

llvm::Value* codegen(const MemberExpr& expr) {
    auto* value = codegenLvalue(expr);
    return value->getType()->isPointerTy() ? builder.CreateLoad(value) : value;
}

llvm::Value* codegenLvalue(const SubscriptExpr& expr) {
    auto* value = codegenLvalue(*expr.array);
    if (value->getType()->getPointerElementType()->isPointerTy()) value = builder.CreateLoad(value);
    return builder.CreateGEP(value,
                             {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0), codegen(*expr.index)});
}

llvm::Value* codegen(const SubscriptExpr& expr) {
    return builder.CreateLoad(codegenLvalue(expr));
}

llvm::Value* codegen(const Expr& expr) {
    switch (expr.getKind()) {
        case ExprKind::VariableExpr:    return codegen(expr.getVariableExpr());
        case ExprKind::StrLiteralExpr:  return codegen(expr.getStrLiteralExpr(), expr);
        case ExprKind::IntLiteralExpr:  return codegen(expr.getIntLiteralExpr(), expr);
        case ExprKind::BoolLiteralExpr: return codegen(expr.getBoolLiteralExpr());
        case ExprKind::NullLiteralExpr: return codegen(expr.getNullLiteralExpr(), expr);
        case ExprKind::ArrayLiteralExpr:return codegen(expr.getArrayLiteralExpr());
        case ExprKind::PrefixExpr:      return codegen(expr.getPrefixExpr());
        case ExprKind::BinaryExpr:      return codegen(expr.getBinaryExpr());
        case ExprKind::CallExpr:        return codegen(expr.getCallExpr());
        case ExprKind::CastExpr:        return codegen(expr.getCastExpr());
        case ExprKind::MemberExpr:      return codegen(expr.getMemberExpr());
        case ExprKind::SubscriptExpr:   return codegen(expr.getSubscriptExpr());
    }
}

llvm::Value* codegenLvalue(const Expr& expr) {
    switch (expr.getKind()) {
        case ExprKind::VariableExpr:    return codegenLvalue(expr.getVariableExpr());
        case ExprKind::StrLiteralExpr:  llvm_unreachable("no lvalue string literals");
        case ExprKind::IntLiteralExpr:  llvm_unreachable("no lvalue integer literals");
        case ExprKind::BoolLiteralExpr: llvm_unreachable("no lvalue boolean literals");
        case ExprKind::NullLiteralExpr: llvm_unreachable("no lvalue null literals");
        case ExprKind::ArrayLiteralExpr:llvm_unreachable("no lvalue array literals");
        case ExprKind::PrefixExpr:      return codegenLvalue(expr.getPrefixExpr());
        case ExprKind::BinaryExpr:      llvm_unreachable("no lvalue binary expressions");
        case ExprKind::CallExpr:        llvm_unreachable("IRGen doesn't support lvalue call expressions yet");
        case ExprKind::CastExpr:        llvm_unreachable("IRGen doesn't support lvalue cast expressions yet");
        case ExprKind::MemberExpr:      return codegenLvalue(expr.getMemberExpr());
        case ExprKind::SubscriptExpr:   return codegenLvalue(expr.getSubscriptExpr());
    }
}

void beginScope() {
    scopes.emplace_back();
}

void endScope() {
    scopes.back().onScopeEnd();
    scopes.pop_back();
}

void deferEvaluationOf(const Expr& expr) {
    scopes.back().deferredExprs.emplace_back(&expr);
}

void deferDeinitCallOf(llvm::Value* value) {
    scopes.back().valuesToDeinit.emplace_back(value);
}

void codegenDeferredExprsAndDeinitCallsForReturn() {
    for (auto& scope : llvm::reverse(scopes)) scope.onScopeEnd();
    scopes.back().clear();
}

void codegen(const ReturnStmt& stmt) {
    assert(stmt.values.size() < 2 && "IRGen doesn't support multiple return values yet");

    codegenDeferredExprsAndDeinitCallsForReturn();

    if (stmt.values.empty()) {
        if (currentDecl->getFuncDecl().name != "main") builder.CreateRetVoid();
        else builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0));
    } else {
        builder.CreateRet(codegen(*stmt.values[0]));
    }
}

llvm::AllocaInst* createEntryBlockAlloca(Type deltaType, llvm::Type* type,
                                         llvm::Value* arraySize = nullptr,
                                         const llvm::Twine& name = "") {
    auto* insertBlock = builder.GetInsertBlock();
    if (lastAlloca == llvm::BasicBlock::iterator()) {
        if (insertBlock->getParent()->getEntryBlock().empty()) {
            builder.SetInsertPoint(&insertBlock->getParent()->getEntryBlock());
        } else {
            builder.SetInsertPoint(&insertBlock->getParent()->getEntryBlock().front());
        }
    } else {
        builder.SetInsertPoint(&insertBlock->getParent()->getEntryBlock(), std::next(lastAlloca));
    }
    auto* alloca = builder.CreateAlloca(type, arraySize, name);
    lastAlloca = alloca->getIterator();
    setLocalValue(deltaType, name.str(), alloca);
    builder.SetInsertPoint(insertBlock);
    return alloca;
}

void codegen(const VariableStmt& stmt) {
    auto* alloca = createEntryBlockAlloca(stmt.decl->getType(), toIR(stmt.decl->getType()),
                                          nullptr, stmt.decl->name);
    if (auto initializer = stmt.decl->initializer) {
        builder.CreateStore(codegenForPassing(*initializer), alloca);
    }
}

void codegen(const IncrementStmt& stmt) {
    auto* alloca = codegenLvalue(*stmt.operand);
    auto* value = builder.CreateLoad(alloca);
    auto* result = builder.CreateAdd(value, llvm::ConstantInt::get(value->getType(), 1));
    builder.CreateStore(result, alloca);
}

void codegen(const DecrementStmt& stmt) {
    auto* alloca = codegenLvalue(*stmt.operand);
    auto* value = builder.CreateLoad(alloca);
    auto* result = builder.CreateSub(value, llvm::ConstantInt::get(value->getType(), 1));
    builder.CreateStore(result, alloca);
}

void codegen(const Stmt& stmt);

void codegen(const IfStmt& ifStmt) {
    auto* condition = codegen(*ifStmt.condition);
    auto* func = builder.GetInsertBlock()->getParent();
    auto* thenBlock = llvm::BasicBlock::Create(ctx, "then", func);
    auto* elseBlock = llvm::BasicBlock::Create(ctx, "else", func);
    auto* endIfBlock = llvm::BasicBlock::Create(ctx, "endif", func);
    breakTargets.push_back(endIfBlock);
    builder.CreateCondBr(condition, thenBlock, elseBlock);

    builder.SetInsertPoint(thenBlock);
    beginScope();
    for (const auto& stmt : ifStmt.thenBody) {
        codegen(*stmt);
        if (stmt->isReturnStmt()) break;
    }
    endScope();
    if (thenBlock->empty() || !llvm::isa<llvm::ReturnInst>(thenBlock->back()))
        builder.CreateBr(endIfBlock);

    builder.SetInsertPoint(elseBlock);
    beginScope();
    for (const auto& stmt : ifStmt.elseBody) {
        codegen(*stmt);
        if (stmt->isReturnStmt()) break;
    }
    endScope();
    if (elseBlock->empty() || !llvm::isa<llvm::ReturnInst>(elseBlock->back()))
        builder.CreateBr(endIfBlock);

    breakTargets.pop_back();
    builder.SetInsertPoint(endIfBlock);
}

void codegen(const SwitchStmt& switchStmt) {
    auto* condition = codegen(*switchStmt.condition);
    auto* func = builder.GetInsertBlock()->getParent();
    auto* insertBlockBackup = builder.GetInsertBlock();

    std::vector<std::pair<llvm::ConstantInt*, llvm::BasicBlock*>> cases;
    for (const SwitchCase& switchCase : switchStmt.cases) {
        auto* value = llvm::cast<llvm::ConstantInt>(codegen(*switchCase.value));
        auto* block = llvm::BasicBlock::Create(ctx, "", func);
        cases.emplace_back(value, block);
    }

    builder.SetInsertPoint(insertBlockBackup);
    auto* defaultBlock = llvm::BasicBlock::Create(ctx, "default", func);
    auto* end = llvm::BasicBlock::Create(ctx, "endswitch", func);
    breakTargets.push_back(end);
    auto* switchInst = builder.CreateSwitch(condition, defaultBlock);

    auto casesIterator = cases.begin();
    for (const SwitchCase& switchCase : switchStmt.cases) {
        auto* value = casesIterator->first;
        auto* block = casesIterator->second;

        builder.SetInsertPoint(block);
        beginScope();
        for (const auto& stmt : switchCase.stmts) {
            codegen(*stmt);
            if (stmt->isReturnStmt() || stmt->isBreakStmt()) break;
        }
        endScope();

        if (block->empty() || (!llvm::isa<llvm::ReturnInst>(block->back())
                            && !llvm::isa<llvm::BranchInst>(block->back())))
            builder.CreateBr(end);

        switchInst->addCase(value, block);
        ++casesIterator;
    }

    { // Codegen the default block.
        builder.SetInsertPoint(defaultBlock);
        beginScope();
        for (const auto& stmt : switchStmt.defaultStmts) {
            codegen(*stmt);
            if (stmt->isReturnStmt()) break;
        }
        endScope();
        if (defaultBlock->empty() || !llvm::isa<llvm::ReturnInst>(defaultBlock->back()))
            builder.CreateBr(end);
    }

    breakTargets.pop_back();
    builder.SetInsertPoint(end);
}

void codegen(const WhileStmt& whileStmt) {
    auto* func = builder.GetInsertBlock()->getParent();
    auto* cond = llvm::BasicBlock::Create(ctx, "while", func);
    auto* body = llvm::BasicBlock::Create(ctx, "body", func);
    auto* end = llvm::BasicBlock::Create(ctx, "endwhile", func);
    breakTargets.push_back(end);
    builder.CreateBr(cond);

    builder.SetInsertPoint(cond);
    builder.CreateCondBr(codegen(*whileStmt.condition), body, end);

    builder.SetInsertPoint(body);
    beginScope();
    for (const auto& stmt : whileStmt.body) {
        codegen(*stmt);
        if (stmt->isReturnStmt()) break;
    }
    endScope();
    if (body->empty() || !llvm::isa<llvm::ReturnInst>(body->back()))
        builder.CreateBr(cond);

    breakTargets.pop_back();
    builder.SetInsertPoint(end);
}

void codegen(const BreakStmt& breakStmt) {
    assert(!breakTargets.empty());
    builder.CreateBr(breakTargets.back());
}

void codegen(const AssignStmt& stmt) {
    auto* lhs = codegenLvalue(*stmt.lhs);
    builder.CreateStore(codegen(*stmt.rhs), lhs);
}

void codegen(const AugAssignStmt& stmt) {
    switch (stmt.op.rawValue) {
        case AND_AND: fatalError("'&&=' not implemented yet");
        case OR_OR:   fatalError("'||=' not implemented yet");
    }
    auto* lhs = codegenLvalue(*stmt.lhs);
    auto* rhs = codegen(*stmt.rhs);
    auto* result = codegenBinaryOp(stmt.op, builder.CreateLoad(lhs), rhs, *stmt.lhs);
    builder.CreateStore(result, lhs);
}

void codegen(const Stmt& stmt) {
    switch (stmt.getKind()) {
        case StmtKind::ReturnStmt:    codegen(stmt.getReturnStmt()); break;
        case StmtKind::VariableStmt:  codegen(stmt.getVariableStmt()); break;
        case StmtKind::IncrementStmt: codegen(stmt.getIncrementStmt()); break;
        case StmtKind::DecrementStmt: codegen(stmt.getDecrementStmt()); break;
        case StmtKind::ExprStmt:      codegen(*stmt.getExprStmt().expr); break;
        case StmtKind::DeferStmt:     deferEvaluationOf(*stmt.getDeferStmt().expr); break;
        case StmtKind::IfStmt:        codegen(stmt.getIfStmt()); break;
        case StmtKind::SwitchStmt:    codegen(stmt.getSwitchStmt()); break;
        case StmtKind::WhileStmt:     codegen(stmt.getWhileStmt()); break;
        case StmtKind::BreakStmt:     codegen(stmt.getBreakStmt()); break;
        case StmtKind::AssignStmt:    codegen(stmt.getAssignStmt()); break;
        case StmtKind::AugAssignStmt: codegen(stmt.getAugAssignStmt()); break;
    }
}

void createDeinitCall(llvm::Value* valueToDeinit) {
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(valueToDeinit);
    auto* typeToDeinit = alloca ? alloca->getAllocatedType() : valueToDeinit->getType();
    if (!typeToDeinit->isStructTy()) return;

    // Prevent recursively destroying the argument in struct deinitializers.
    if (llvm::isa<llvm::Argument>(valueToDeinit)
        && builder.GetInsertBlock()->getParent()->getName().endswith(".deinit")) return;

    llvm::StringRef typeName = typeToDeinit->getStructName();
    llvm::Function* deinit = module.getFunction(mangleDeinitDecl(typeName));
    if (!deinit) return;
    if (valueToDeinit->getType()->isPointerTy() && !deinit->arg_begin()->getType()->isPointerTy()) {
        builder.CreateCall(deinit, builder.CreateLoad(valueToDeinit));
    } else if (!valueToDeinit->getType()->isPointerTy() && deinit->arg_begin()->getType()->isPointerTy()) {
        llvm::errs() << "deinitialization of by-value class parameters not implemented yet\n";
        abort();
    } else {
        builder.CreateCall(deinit, valueToDeinit);
    }
}

llvm::Type* getLLVMTypeForPassing(llvm::StringRef typeName) {
    auto structTypeAndDecl = structs.find(typeName)->second;
    if (structTypeAndDecl.second->passByValue()) {
        return structTypeAndDecl.first;
    } else {
        return llvm::PointerType::get(structTypeAndDecl.first, 0);
    }
}

llvm::Function* codegenFuncProto(const FuncDecl& decl, llvm::StringRef mangledName = {},
                                 bool addToFuncs = true) {
    auto* funcType = decl.getFuncType();

    assert(!funcType->returnType.isTupleType() && "IRGen doesn't support tuple return values yet");
    auto* returnType = toIR(funcType->returnType);
    if (decl.name == "main" && returnType->isVoidTy()) returnType = llvm::Type::getInt32Ty(ctx);

    llvm::SmallVector<llvm::Type*, 16> paramTypes;
    if (decl.isMemberFunc()) paramTypes.emplace_back(getLLVMTypeForPassing(decl.receiverType));
    for (const auto& t : funcType->paramTypes) paramTypes.emplace_back(toIR(t));

    auto* llvmFuncType = llvm::FunctionType::get(returnType, paramTypes, false);
    if (mangledName.empty()) mangledName = decl.name;
    auto* func = llvm::Function::Create(llvmFuncType, llvm::Function::ExternalLinkage,
                                        mangledName, &module);

    auto arg = func->arg_begin(), argsEnd = func->arg_end();
    if (decl.isMemberFunc()) arg++->setName("this");
    for (auto param = decl.params.begin(); arg != argsEnd; ++param, ++arg) arg->setName(param->name);

    if (addToFuncs) funcs.emplace(decl.name, func);
    return func;
}

llvm::Function* codegenGenericFuncProto(const GenericFuncDecl& decl, llvm::ArrayRef<Type> genericArgs) {
    assert(decl.genericParams.size() == genericArgs.size());
    auto genericArg = genericArgs.begin();
    for (const GenericParamDecl& genericParam : decl.genericParams) {
        currentGenericArgs.insert({genericParam.name, toIR(*genericArg)});
    }
    auto proto = codegenFuncProto(*decl.func, mangle(decl, genericArgs), false);
    currentGenericArgs.clear();
    return proto;
}

llvm::Function* getFuncForCall(const CallExpr& call) {
    auto it = funcs.find(call.funcName);
    if (it == funcs.end()) {
        // Function has not been declared yet, search for it in the symbol table.
        const Decl& decl = findInSymbolTable(call.funcName, call.srcLoc);
        if (decl.isFuncDecl())
            return codegenFuncProto(decl.getFuncDecl());
        else {
            auto it = llvm::find_if(genericFuncInstantiations,
                                    [&call](GenericFuncInstantiation& instantiation) {
                return instantiation.decl.func->name == call.funcName
                    && instantiation.genericArgs == llvm::ArrayRef<Type>(call.genericArgs);
            });
            if (it != genericFuncInstantiations.end()) return it->func;
            auto* func = codegenGenericFuncProto(decl.getGenericFuncDecl(), call.genericArgs);
            genericFuncInstantiations.emplace_back(GenericFuncInstantiation{
                decl.getGenericFuncDecl(), call.genericArgs, func
            });
            return func;
        }
    }
    return it->second;
}

void codegenFuncBody(const FuncDecl& decl, llvm::Function& func) {
    lastAlloca = llvm::BasicBlock::iterator();
    builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "", &func));
    beginScope();
    auto arg = func.arg_begin();
    if (decl.isMemberFunc()) setLocalValue(nullptr, "this", &*arg++);
    for (const auto& param : decl.params) setLocalValue(param.type, param.name, &*arg++);
    for (const auto& stmt : *decl.body) codegen(*stmt);
    endScope();

    if (builder.GetInsertBlock()->empty() || !llvm::isa<llvm::ReturnInst>(builder.GetInsertBlock()->back())) {
        if (func.getName() != "main") {
            builder.CreateRetVoid();
        } else {
            builder.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0));
        }
    }
    localValues.clear();
}

void codegen(const FuncDecl& decl) {
    auto it = funcs.find(decl.name);
    auto* func = it == funcs.end() ? codegenFuncProto(decl) : it->second;
    if (!decl.isExtern()) codegenFuncBody(decl, *func);
    assert(!llvm::verifyFunction(*func, &llvm::errs()));
}

void codegen(const InitDecl& decl) {
    FuncDecl funcDecl{mangle(decl), decl.params, decl.getTypeDecl().getType(),
        "", nullptr, SrcLoc::invalid()};
    auto* func = codegenFuncProto(funcDecl);
    builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "", func));

    auto* type = llvm::cast<llvm::StructType>(toIR(decl.getTypeDecl().getType()));
    auto* alloca = builder.CreateAlloca(type);

    setLocalValue(funcDecl.returnType, "this", alloca);
    auto param = decl.params.begin();
    for (auto& arg : func->args()) setLocalValue(param++->type, arg.getName(), &arg);
    for (const auto& stmt : *decl.body) codegen(*stmt);
    builder.CreateRet(builder.CreateLoad(alloca));
    localValues.clear();

    assert(!llvm::verifyFunction(*func, &llvm::errs()));
}

void codegen(const DeinitDecl& decl) {
    FuncDecl funcDecl{mangle(decl), {}, Type::getVoid(),
        decl.getTypeDecl().name, decl.body, decl.srcLoc};
    llvm::Function* func = codegenFuncProto(funcDecl);
    codegenFuncBody(funcDecl, *func);
    assert(!llvm::verifyFunction(*func, &llvm::errs()));
}

void codegen(const TypeDecl& decl) {
    if (decl.fields.empty()) {
        structs.emplace(decl.name, std::make_pair(llvm::StructType::get(ctx), &decl));
        return;
    }
    auto elements = map(decl.fields, *[](const FieldDecl& f) { return toIR(f.type); });
    structs.emplace(decl.name, std::make_pair(llvm::StructType::create(elements, decl.name), &decl));
}

void codegen(const VarDecl& decl) {
    auto* value = new llvm::GlobalVariable(module, toIR(decl.getType()), !decl.isMutable(),
                                           llvm::GlobalValue::PrivateLinkage,
                                           llvm::cast<llvm::Constant>(codegen(*decl.initializer)),
                                           decl.name);
    globalValues.emplace(decl.name, value);
}

void codegen(const Decl& decl) {
    switch (decl.getKind()) {
        case DeclKind::ParamDecl: llvm_unreachable("handled via FuncDecl");
        case DeclKind::FuncDecl:  codegen(decl.getFuncDecl()); break;
        case DeclKind::GenericParamDecl: llvm_unreachable("cannot codegen generic parameter declaration");
        case DeclKind::GenericFuncDecl: break; // Cannot codegen uninstantiated generic function.
        case DeclKind::InitDecl:  codegen(decl.getInitDecl()); break;
        case DeclKind::DeinitDecl:codegen(decl.getDeinitDecl()); break;
        case DeclKind::TypeDecl:  codegen(decl.getTypeDecl()); break;
        case DeclKind::VarDecl:   codegen(decl.getVarDecl()); break;
        case DeclKind::FieldDecl: llvm_unreachable("handled via TypeDecl");
        case DeclKind::ImportDecl: break;
    }
}

} // anonymous namespace

llvm::Module& irgen::compile(const std::vector<Decl>& decls) {
    globalDecls = &decls;
    for (const Decl& decl : decls) {
        currentDecl = &decl;
        codegen(decl);
    }

    for (const GenericFuncInstantiation& instantiation : genericFuncInstantiations) {
        auto genericArg = instantiation.genericArgs.begin();
        for (const GenericParamDecl& genericParam : instantiation.decl.genericParams) {
            currentGenericArgs.insert({genericParam.name, toIR(*genericArg)});
        }
        codegenFuncBody(*instantiation.decl.func, *instantiation.func);
        assert(!llvm::verifyFunction(*instantiation.func, &llvm::errs()));
        currentGenericArgs.clear();
    }

    assert(!llvm::verifyModule(module, &llvm::errs()));
    return module;
}
