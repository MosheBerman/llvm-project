//===--- NullabilityAnnotatorCheck.cpp - clang-tidy -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NullabilityAnnotatorCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ComputeDependence.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::objc {

const Expr *getInnermostExpr(Expr *Exp) {
  const clang::Expr *E = Exp;
  E = E->IgnoreCasts()->IgnoreImpCasts();

  if (const ExprWithCleanups *EWCU = dyn_cast<ExprWithCleanups>(E)) {
    llvm::errs() << "Is cleanup expr."
                 << "\n";
    E = EWCU->getSubExpr();
  }

  if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    E = getInnermostExpr(const_cast<Expr *>(E->IgnoreImpCasts()));
  }

  if (const MaterializeTemporaryExpr *M = dyn_cast<MaterializeTemporaryExpr>(E))
    E = M->getSubExpr();

  while (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E))
    E = ICE->getSubExprAsWritten();

  return E;
}

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
  // Method decls
  Finder->addMatcher(
      traverse(TK_IgnoreUnlessSpelledInSource, objcMethodDecl().bind("omd")),
      this);

  // Function decls
  Finder->addMatcher(
      traverse(TK_IgnoreUnlessSpelledInSource, functionDecl().bind("fd")),
      this);

  // Property Decls
  Finder->addMatcher(
      traverse(TK_IgnoreUnlessSpelledInSource, objcPropertyDecl().bind("opd")),
      this);

  // Global variables
  // Finder->addMatcher(.bind("gvd"), this);
}

bool isSomeKindOfNil(const Expr *E, ASTContext *C) {
  if (!E) {
    return false;
  }
  // Zero literal
  const auto *const IL = dyn_cast_or_null<IntegerLiteral>(E);
  const auto NullPtrConstKind =
      E->isNullPointerConstant(*C, Expr::NullPointerConstantValueDependence());

  // nil/null/nullptr/NULL
  // See: https://nshipster.com/nil/
  return (NullPtrConstKind !=
          Expr::NullPointerConstantKind::NPCK_NotNull) /* Pointer is not
                                        nonnull, then it's some form of
                                        nil/null
                                      */
         || (IL && IL->getValue() == 0)                /* Zero literal */
         || isa<GNUNullExpr>(E)                        /* __null */
         || isa<CXXNullPtrLiteralExpr>(E) /* nullptr */;
}

/// Determine if a return statement's value is `nil`, nullable, or a nonnull
/// value.
NullabilityKind getNullabilityOfReturnStmt(ReturnStmt *RS, ASTContext &Ctx) {
  if (!RS) {
    llvm::errs()
        << "`getNullabilityOfReturnStmt` expected a `ReturnStmt` pointer. "
           "Instead, got `nullptr`.";
    return NullabilityKind::Unspecified;
  }

  Expr *RV = RS->getRetValue();

  if (!RV) {
    // Return statements are allowed in void methods.
    return NullabilityKind::Unspecified;
  }

  Expr *RVIgnoringCasts = const_cast<Expr *>(getInnermostExpr(RV));

  // Void functions/methods without a return stmt.
  // base case: return `nil` literal
  // https://stackoverflow.com/a/38194354
  if ((RVIgnoringCasts && isSomeKindOfNil(RVIgnoringCasts, &Ctx)) ||
      isSomeKindOfNil(RV, &Ctx)) {
    return NullabilityKind::Nullable;
  }

  // base case: return string literal
  if (isa<ObjCStringLiteral>(RV) || isa<ObjCStringLiteral>(RVIgnoringCasts)) {
    return NullabilityKind::NonNull;
  }

  // If the return statement makes a call, check nullability of the call.
  ObjCMessageExpr *const OME =
      dyn_cast_or_null<ObjCMessageExpr>(RVIgnoringCasts);
  if (OME) {
    QualType MsgRetType = OME->getCallReturnType(Ctx);
    std::optional<NullabilityKind> NK = MsgRetType->getNullability();
    if (NK) {
      return *NK;
    }
  }

  DeclRefExpr *const DRE = dyn_cast_or_null<DeclRefExpr>(RVIgnoringCasts);
  if (DRE) {
    QualType QT = DRE->getType();
    std::optional<NullabilityKind> NK = QT->getNullability();
    if (NK) {
      return *NK;
    }
  }

  // If the return statement makes a call, check nullability of the call.
  CallExpr *const CE = dyn_cast_or_null<CallExpr>(RVIgnoringCasts);
  if (CE) {
    QualType QT = CE->getCallReturnType(Ctx);
    std::optional<NullabilityKind> NK = QT->getNullability();
    if (NK) {
      return *NK;
    }
  }
  return NullabilityKind::Unspecified;
}

/// Given a set of return statements, find the weakest nullability between all
/// of them.
/// Assume "strongest" nullability, unless the are no return statements. We
/// could have also assumed weakest and checked for "greater" nullability, but
/// `hasWeakerNullability` was already defined in `Specifiers.h` when I found
/// this.
std::optional<NullabilityKind>
getWeakestNullabilityForReturnStatements(ReturnStatementCollector Visitor,
                                         ASTContext *Ctx) {

  std::vector<ReturnStmt *> ReturnStatements = Visitor.getVisited();
  NullabilityKind NK = NullabilityKind::NonNull;

  // Return unspecified if no return statements were found by the visitor.
  if (ReturnStatements.empty()) {
    return std::nullopt;
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
void NullabilityAnnotatorCheck::check(const MatchFinder::MatchResult &Result) {
  ReturnStatementCollector Visitor;

  const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("fd");
  const ObjCMethodDecl *OMD = Result.Nodes.getNodeAs<ObjCMethodDecl>("omd");

  std::string Name = "<unnamed>";

  if (FD != nullptr) {
    Visitor.TraverseFunctionDecl(const_cast<FunctionDecl *>(FD));
    Name = FD->getQualifiedNameAsString();
  } else if (OMD != nullptr) {
    Visitor.TraverseObjCMethodDecl(const_cast<ObjCMethodDecl *>(OMD));
    Name = OMD->getQualifiedNameAsString();
  }
  std::optional<NullabilityKind> WeakestNullability =
      getWeakestNullabilityForReturnStatements(Visitor, Result.Context);
  if (WeakestNullability) {
    llvm::errs() << "WeakestNullability of " << Name << " is "
                 << getNullabilitySpelling(*WeakestNullability, true) << "\n";
  } else {
    llvm::errs() << "" << Name << " has no return stmts."
                 << "\n";
  }
}

} // namespace clang::tidy::objc
