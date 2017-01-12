#include <iostream>
#include <string>
#include "ast_printer.h"
#include "../ast/decl.h"
#include "../ast/stmt.h"

static int indentLevel = 0;

/// Inserts a line break followed by appropriate indentation.
static std::ostream& br(std::ostream& out) {
    return out << '\n' << std::string(indentLevel * 4, ' ');
}

std::ostream& operator<<(std::ostream& out, const Expr& expr);
std::ostream& operator<<(std::ostream& out, const Stmt& stmt);

std::ostream& operator<<(std::ostream& out, const VariableExpr& expr) {
    return out << expr.identifier;
}

std::ostream& operator<<(std::ostream& out, const StrLiteralExpr& expr) {
    return out << '"' << expr.value << '"';
}

std::ostream& operator<<(std::ostream& out, const IntLiteralExpr& expr) {
    return out << expr.value;
}

std::ostream& operator<<(std::ostream& out, const BoolLiteralExpr& expr) {
    return out << (expr.value ? "true" : "false");
}

std::ostream& operator<<(std::ostream& out, const PrefixExpr& expr) {
    return out << "(" << expr.op << *expr.operand << ")";
}

std::ostream& operator<<(std::ostream& out, const BinaryExpr& expr) {
    return out << "(" << *expr.left << " " << expr.op << " " << *expr.right << ")";
}

std::ostream& operator<<(std::ostream& out, const CallExpr& expr) {
    out << "(call " << expr.funcName << " ";
    for (const Expr& arg : expr.args) {
        out << arg;
        if (&arg != &expr.args.back()) out << " ";
    }
    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const Expr& expr) {
    switch (expr.getKind()) {
        case ExprKind::VariableExpr:   return out << expr.getVariableExpr();
        case ExprKind::StrLiteralExpr: return out << expr.getStrLiteralExpr();
        case ExprKind::IntLiteralExpr: return out << expr.getIntLiteralExpr();
        case ExprKind::BoolLiteralExpr:return out << expr.getBoolLiteralExpr();
        case ExprKind::PrefixExpr:     return out << expr.getPrefixExpr();
        case ExprKind::BinaryExpr:     return out << expr.getBinaryExpr();
        case ExprKind::CallExpr:       return out << expr.getCallExpr();
    }
}

std::ostream& operator<<(std::ostream& out, const ReturnStmt& stmt) {
    out << br << "(return-stmt ";
    for (const Expr& value : stmt.values) {
        out << value;
        if (&value != &stmt.values.back()) out << " ";
    }
    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const VariableStmt& stmt) {
    return out << br << "(var-stmt " << stmt.decl->name << " " << *stmt.decl->initializer << ")";
}

std::ostream& operator<<(std::ostream& out, const IncrementStmt& stmt) {
    return out << br << "(inc-stmt " << stmt.operand << ")";
}

std::ostream& operator<<(std::ostream& out, const DecrementStmt& stmt) {
    return out << br << "(dec-stmt " << stmt.operand << ")";
}

std::ostream& operator<<(std::ostream& out, const IfStmt& stmt) {
    out << br << "(if-stmt " << stmt.condition;
    indentLevel++;
    out << br << "(then";
    indentLevel++;
    for (const Stmt& substmt : stmt.thenBody) {
        out << br << substmt;
    }
    out << ")";
    indentLevel--;
    out << br << "(else";
    indentLevel++;
    for (const Stmt& substmt : stmt.elseBody) {
        if (substmt.getKind() != StmtKind::IfStmt) out << br;
        out << substmt;
    }
    indentLevel--;
    out << ")";
    indentLevel--;
    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const WhileStmt& stmt) {
    out << br << "(while-stmt " << stmt.condition;
    indentLevel++;
    for (const Stmt& substmt : stmt.body) {
        out << br << substmt;
    }
    indentLevel--;
    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const AssignStmt& stmt) {
    return out << br << "(assign-stmt " << stmt.lhs << " " << stmt.rhs << ")";
}

std::ostream& operator<<(std::ostream& out, const Stmt& stmt) {
    switch (stmt.getKind()) {
        case StmtKind::ReturnStmt:    return out << stmt.getReturnStmt();
        case StmtKind::VariableStmt:  return out << stmt.getVariableStmt();
        case StmtKind::IncrementStmt: return out << stmt.getIncrementStmt();
        case StmtKind::DecrementStmt: return out << stmt.getDecrementStmt();
        case StmtKind::CallStmt:      return out << stmt.getCallStmt().expr;
        case StmtKind::IfStmt:        return out << stmt.getIfStmt();
        case StmtKind::WhileStmt:     return out << stmt.getWhileStmt();
        case StmtKind::AssignStmt:    return out << stmt.getAssignStmt();
    }
}

std::ostream& operator<<(std::ostream& out, const ParamDecl& decl) {
    return out << "(" << decl.type << " " << decl.name << ")";
}

std::ostream& operator<<(std::ostream& out, const FuncDecl& decl) {
    out << br << (decl.isExtern() ? "(extern-func-decl " : "(func-decl ");
    out << decl.name << " (";
    for (const ParamDecl& param : decl.params) {
        out << param;
        if (&param != &decl.params.back()) out << " ";
    }
    out << ") " << decl.returnType;

    if (!decl.isExtern()) {
        indentLevel++;
        for (const Stmt& stmt : *decl.body) {
            out << stmt;
        }
        indentLevel--;
    }

    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const FieldDecl& decl) {
    return out << br << "(field-decl " << decl.type << " " << decl.name << ")";
}

std::ostream& operator<<(std::ostream& out, const TypeDecl& decl) {
    out << br << "(type-decl ";
    switch (decl.tag) {
        case TypeTag::Struct: out << "struct "; break;
        case TypeTag::Class: out << "class "; break;
    }
    out << decl.name;
    indentLevel++;
    for (const FieldDecl& field : decl.fields) {
        out << field;
    }
    indentLevel--;
    return out << ")";
}

std::ostream& operator<<(std::ostream& out, const VarDecl& decl) {
    return out << br << "(var-decl " << decl.name << " " << *decl.initializer << ")";
}

std::ostream& operator<<(std::ostream& out, const Decl& decl) {
    switch (decl.getKind()) {
        case DeclKind::ParamDecl: return out << decl.getParamDecl();
        case DeclKind::FuncDecl:  return out << decl.getFuncDecl();
        case DeclKind::TypeDecl:  return out << decl.getTypeDecl();
        case DeclKind::VarDecl:   return out << decl.getVarDecl();
        case DeclKind::FieldDecl: return out << decl.getFieldDecl();
    }
}

std::ostream& operator<<(std::ostream& out, const AST& decls) {
    out << "(source-file";
    indentLevel++;
    for (const Decl& decl : decls) {
        out << decl;
    }
    indentLevel--;
    return out << ")\n";
}
