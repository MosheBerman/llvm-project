//===--- NullabilityAnnotatorCheck.cpp - clang-tidy -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NullabilityAnnotatorCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::objc {

/// Collects return statements to find how "nullable" they are.
class ReturnStatementCollector
    : public RecursiveASTVisitor<ReturnStatementCollector> {
  using Base = RecursiveASTVisitor<ReturnStatementCollector>;

private:
  std::vector<ReturnStmt *> Visited = std::vector<ReturnStmt *>();

public:
  ReturnStatementCollector() {}

  /// Collect each visited return statement.
  bool VisitReturnStmt(ReturnStmt *RS) {
    Visited.push_back(RS);
    return true;
  }

  /// Access the visited nodes.
  std::vector<ReturnStmt *> getVisited() { return this->Visited; }
};

void NullabilityAnnotatorCheck::registerMatchers(MatchFinder *Finder) {

  Finder->addMatcher(objcMethodDecl().bind("omd"), this);
  Finder->addMatcher(functionDecl().bind("fd"), this);
}

bool isExprNilOrZeroLiteral(const Expr *E) {
  if (!E) {
    return false;
  }
  // Zero literal
  if (const auto *const IL = dyn_cast_or_null<IntegerLiteral>(E)) {
    return IL->getValue() == 0;
  }
  // nil/null/nullptr/NULL
  // See: https://nshipster.com/nil/
  return isa<GNUNullExpr>(E) || isa<CXXNullPtrLiteralExpr>(E);
}

/// Determine if a return statement's value is `nil`, nullable, or a nonnull
/// value.
NullabilityKind getNullabilityOfReturnStmt(ReturnStmt *RS, ASTContext &Ctx) {
  if (!RS) {
    llvm::errs() << "Return statement is null.";
    return NullabilityKind::Unspecified;
  }
  Expr *RV = RS->getRetValue();

  // Remove implicit casts.
  if (RV) {
    RV = RV->IgnoreCasts();
  }
  bool IsStringLiteral = dyn_cast_or_null<ObjCStringLiteral>(RV);

  if (isExprNilOrZeroLiteral(RV)) {
    return NullabilityKind::Nullable;
  }
  if (IsStringLiteral) {
    return NullabilityKind::NonNull;
  }
  return NullabilityKind::Unspecified;
}

/// Given a set of return statements, find the weakest nullability between all
/// of them.
/// Assume "strongest" nullability, unless the are no return statements. We
/// could have also assumed weakest and checked for "greater" nullability, but
/// `hasWeakerNullability` was already defined in `Specifiers.h` when I found
/// this.
NullabilityKind
getWeakestNullabilityForReturnStatements(ReturnStatementCollector Visitor,
                                         ASTContext *Ctx) {

  std::vector<ReturnStmt *> ReturnStatements = Visitor.getVisited();
  NullabilityKind NK = NullabilityKind::NonNull;

  // Return unspecified if no return statements were found by the visitor.
  if (ReturnStatements.empty()) {
    NK = NullabilityKind::Unspecified;
    return NK;
  }
  for (ReturnStmt *RS : ReturnStatements) {
    NullabilityKind NRS = getNullabilityOfReturnStmt(RS, *Ctx);
    if (hasWeakerNullability(NRS, NK)) {
      NK = NRS;
    }
  }
  return NK;
}

/// This check deduces nullability of pointers.
///
/// The first pass deduces nullability of functions and methods, by examining
/// the return statements of each one, and finding weakest annotation.
///
/// Functions and Methods:
///

void NullabilityAnnotatorCheck::check(const MatchFinder::MatchResult &Result) {
  ReturnStatementCollector Visitor;

  const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("fd");
  const ObjCMethodDecl *OMD = Result.Nodes.getNodeAs<ObjCMethodDecl>("omd");
  std::string Name = "<unnamed>";

  if (FD != nullptr) {
    Visitor.TraverseFunctionDecl(const_cast<FunctionDecl *>(FD));
    Name = FD->getNameAsString();
  } else if (OMD != nullptr) {
    Visitor.TraverseObjCMethodDecl(const_cast<ObjCMethodDecl *>(OMD));
    Name = OMD->getNameAsString();
    // OMD->dumpColor();
  }
  NullabilityKind WeakestNullability =
      getWeakestNullabilityForReturnStatements(Visitor, Result.Context);
  llvm::outs() << "WeakestNullability of " << Name << " is "
               << getNullabilitySpelling(WeakestNullability, true) << "\n";
  // for (ReturnStmt *RS : Visitor.getVisited()) {
  //   NullabilityKind NK = getNullabilityOfReturnStmt(RS, *Result.Context);
  //   llvm::outs() << "" << Name << " contains a return stmt that is "
  //                << getNullabilitySpelling(NK, true) << "\n";
  // }
}

} // namespace clang::tidy::objc
