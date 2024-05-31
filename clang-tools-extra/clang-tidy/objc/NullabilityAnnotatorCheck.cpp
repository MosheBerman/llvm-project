//===--- NullabilityAnnotatorCheck.cpp - clang-tidy -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "NullabilityAnnotatorCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
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
#include "llvm/MC/MCInstrDesc.h"
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
  Finder->addMatcher(varDecl(unless(anyOf(hasAncestor(functionDecl()),
                                          hasAncestor(objcMethodDecl()))))
                         .bind("vd"),
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

  // If the return statement references a decl, such as a parameter or property,
  // we check its nullability here.
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

  // If we reach this point, we expect to find no nullability. Since the return
  // of this function is itself not optional, we fall back to unspecified.
  //
  // This is not unlike a human reviewer who may use an explicit unspecified
  // annotation to note that this particular pointer needs further review.
  return NullabilityKind::Unspecified;
}

/// Given a `ReturnStatementCollector`, find the weakest nullability between all
/// of the return statements that it has collected. These return statements may
/// come from a single function, method, or multiple redclarations of the same
/// one.
///
/// Assume "strongest" nullability, unless the are no return statements. We
/// could have also assumed weakest and checked for "greater" nullability, but
/// `hasWeakerNullability` was already defined in `Specifiers.h` when I found
/// this.
std::optional<NullabilityKind> getWeakestNullabilityForReturnStatements(
    std::vector<ReturnStmt *> ReturnStatements, ASTContext *Ctx) {

  // Return unspecified if no return statements were found by the visitor.
  if (ReturnStatements.empty()) {
    return std::nullopt;
  }
  NullabilityKind NK = NullabilityKind::NonNull;
  for (ReturnStmt *RS : ReturnStatements) {
    NullabilityKind NRS = getNullabilityOfReturnStmt(RS, *Ctx);
    if (hasWeakerNullability(NRS, NK)) {
      NK = NRS;
    }
  }
  return NK;
}

template <typename T>
std::vector<ReturnStmt *> returnStatementsForCanonicalDecl(T DeclOfType) {
  ReturnStatementCollector Visitor;
  std::string Name = DeclOfType->getQualifiedNameAsString();
  bool HasBody = DeclOfType->hasBody();
  bool IsCanonical = DeclOfType->isCanonicalDecl();

  if (!HasBody) {
    // Find all redecls and get weakest return across all of them.
    // This addresses the functions prototypes with multiple implementations,
    // by making the prototype follow the weakest nullability across all
    // implementations.
    for (const auto R : DeclOfType->redecls()) {
      Visitor.TraverseDecl(R);
    }
  } else if (IsCanonical) {
    // Only visit a function with a body if it has no prototype.
    // Otherwise we'll cover it twice, given the above branch.
    Visitor.TraverseDecl(DeclOfType);
  }
  return Visitor.getVisited();
}

/// This check deduces nullability of pointers.
///
/// The first pass deduces nullability of functions and methods, by examining
/// the return statements of each one, and finding weakest annotation.
///
/// To handle interfaces and redeclarations, we will want to find the canonical
/// declaration, and then find all redeclerations in the translation unit.
void NullabilityAnnotatorCheck::check(const MatchFinder::MatchResult &Result) {
  ReturnStatementCollector Visitor;

  const VarDecl *VD = Result.Nodes.getNodeAs<VarDecl>("vd");
  const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("fd");
  const ObjCMethodDecl *OMD = Result.Nodes.getNodeAs<ObjCMethodDecl>("omd");
  std::vector<ReturnStmt *> ReturnStatements;

  std::string Name = "<unnamed>";
  bool HasBody = false;
  bool IsCanonical = false;

  if (VD != nullptr) {
    llvm::errs() << "Found global variable: " << VD->getQualifiedNameAsString()
                 << "\n";
  } else if (FD != nullptr) {
    Name = FD->getQualifiedNameAsString();
    // HasBody = FD->hasBody();
    // IsCanonical = FD->isCanonicalDecl();

    // if (!HasBody) {
    //   // Find all redecls and get weakest return across all of them.
    //   // This addresses the functions prototypes with multiple
    //   implementations,
    //   // by making the prototype follow the weakest nullability across all
    //   // implementations.
    //   for (auto *const R : FD->redecls()) {
    //     Visitor.TraverseFunctionDecl(const_cast<FunctionDecl *>(R));
    //   }
    // } else if (IsCanonical) {
    //   // Only visit a function with a body if it has no prototype.
    //   // Otherwise we'll cover it twice, given the above branch.
    //   Visitor.TraverseFunctionDecl(const_cast<FunctionDecl *>(FD));
    // }

    ReturnStatements = returnStatementsForCanonicalDecl<FunctionDecl *>(
        const_cast<FunctionDecl *>(FD));

  } else if (OMD != nullptr) {
    Name = OMD->getQualifiedNameAsString();
    // HasBody = OMD->hasBody();
    // IsCanonical = OMD->isCanonicalDecl();

    // if (!HasBody) {
    //   // Find all redecls and get weakest return across all of them.
    //   // This addresses the question of an Obj-C protocol with multiple
    //   // implementations, by making the protocol follow the weakest
    //   nullability
    //   // across all implementations.
    //   for (auto *const R : OMD->redecls()) {
    //     Visitor.TraverseObjCMethodDecl(dyn_cast<ObjCMethodDecl>(R));
    //   }
    // } else if (IsCanonical) {
    //   // Only visit a method with a body if it has no prototype.
    //   // Otherwise we'll cover it twice, given the above branch.
    //   Visitor.TraverseObjCMethodDecl(const_cast<ObjCMethodDecl *>(OMD));
    // }
    ReturnStatements = returnStatementsForCanonicalDecl<ObjCMethodDecl *>(
        const_cast<ObjCMethodDecl *>(OMD));
  }

  // By this point, we've collected return statements for all of the
  // implementations of a given canonical decleration. We can now find the
  // weakest nullability and resolve the return type for the decl and its
  // redeclarations.
  if (FD != nullptr || OMD != nullptr) {
    if (!HasBody && IsCanonical) {
      llvm::errs() << "Found prototype for " << Name << "."
                   << "\n";
    } else {

      std::optional<NullabilityKind> WeakestNullability =
          getWeakestNullabilityForReturnStatements(ReturnStatements,
                                                   Result.Context);
      if (WeakestNullability) {
        llvm::errs() << "WeakestNullability of " << Name << " is "
                     << getNullabilitySpelling(*WeakestNullability, true)
                     << "\n";
      } else if (HasBody) {
        llvm::errs() << "" << Name << " has no return stmts."
                     << "\n";
      }
    }
  }
}

} // namespace clang::tidy::objc
