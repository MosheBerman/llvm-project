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
  Finder->addMatcher(
      varDecl(allOf(hasGlobalStorage(),
                    unless(anyOf(hasAncestor(functionDecl()),
                                 hasAncestor(objcMethodDecl())))))
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
/// value, using the following logic for literals:
///       a. A non-null literal value evaluates to `NullabilityKind::NonNull`
///       b. A `nullptr`, `nil`, `NULL` or C-style cast to zero all evaluate to
///          `NullabilityKind::Nullable`.
///
///     We consider calls to other methods, functions, and returns of variables.
///
///       c. If the return value is a `CallExpr` or `ObjCMessageExpr`,
///          we utilize the annotated return type of the function or method
///          being called.
///       d. If the return value is a `DeclRefExpr`, we utilize any annotation
///          on declaration being referenced. (This accounts for returning
///          arguments, variables declared locally to the function/method, and
///          Obj-C instance variables.)
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
  // TODO: We probably want to check for other literals, like `NSNumber`,
  // ()`NSArray`, and `NSDictionary`. `@()`, `@[]` and `@{}`, respectively.)
  // I'm not sure how to import or define these for testing yet.
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

// Find weakest nullability for a function prototype or method interface, by
// considering all the return statements across all redecls. This addresses the
// cases of ObjC protocols and functions prototypes, both with the possibility
// of multiple implementations. We always follow the weakest nullability across
// _all_ implementations.
template <typename T>
std::vector<ReturnStmt *> returnStatementsForCanonicalDecl(T DeclOfType) {
  ReturnStatementCollector Visitor;
  std::string Name = DeclOfType->getQualifiedNameAsString();
  bool HasBody = DeclOfType->hasBody();
  bool IsCanonical = DeclOfType->isCanonicalDecl();

  if (!HasBody) {

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

/// Determine the appropriate nullability for a method argument or function
/// parameter.
///
/// It can be tricky to get this right while avoiding false-determinations. Here
/// are 4 ways *not* to do this:
/// 1. It may be tempting to assume that the existance of an argument means it's
///    there for a reason and choose `NullabilityKind::NonNull` as the correct
///    annotation. This is not what we want because it's common to provide a
///    fallback behavior or branch when the argument is `nil` in a particular
///    call.
///
/// 2. Another naive approach would be to assume any annotation that is checked
///    for `nil` should lead us to determine that `NullabilityKind::Nullable` is
///    appropriate. This can be incorrect in cases where an `IfStmt` is at the
///    top of the scope and the fallback behavior is to return early. This means
///    that the function cannot execute as it otherwise would in the absence of
///    a nonnull value. The correct determination would then be
///    `NullabilityKind:Nonnull`. We can pay more attention to detail.
///
/// 3. When annotating manually, we might be tempted to examine callsites of a
///    particular method or function. In a sense, doing so removes one of the
///    key benefits of nullability annotations. That is, we are no longer
///    setting expectations for callers of our API, and are effectively allowing
///    them to dictate how our code should behave.
///
/// 4. One lazy approach would be to mark arguments as
///    `NullabilityKind::Unspecified`
///    and consider our job done. This will inform developers that they have
///    work to do, while silencing warnings from the "missing annotation"
///    checker. There isn't much benefit to doing this because the checker
///    exists. We can do better.
///
/// 5. A marginally better approach would be to mark arguments as
///    `NullabilityKind::Nullable`
///    and consider our job done. The outcome of this approach is that
///    Swift consumers of our API continue to unwrap all of our newly
///    annotated API. We can do better.
///
///   Unfortunately, it's trickier than return statements to prove the intent of
///   a method or function. We can, however, logically prove certain cases.
///   Let's incorporate the above to annotate arguments and parameters as
///   follows:
///
///   1. When an argument fulfills the following three criteria, it can be
///      reliably annotated as `NullabilityKind::NonNull`.
///      a. It is checked for `nil` before it is otherwise read or
///         written to, *and*
///     b. the `nil` branch does nothing other than exit
///         early, *and*
///     c. the check precedes any other behavior. This condition is necessary to
///        avoid changing the behavior of the code. Although we may accurately
///        determine that a function cannot meaningfully execute if we encounter
///        an early exit, any behavior that occurs prior to the check would no
///        longer execute in the event of a `nil` value at the callsite.
///
///        In this case, it is safe for the developer to delete the `IfStmt`
///        which guards the annotation, assuming they've enabled the
///        "null-passed-to-nonnull" compiler flag as an error. We might not want
///        to do this ourselves, because such checking is still useful in
///        Objective-C when the error is not enabled.
///
///   2. Special case: An Objective-C reference pointer to `NSError` is
///      determined to be `NullabilityKind::Nullable`.
///   (https://developer.apple.com/swift/blog/?id=25)
///
///   3. If an argument is only passed to one or more methods or function, we
///      use the weakest nullability of the annotations in the declaration of
///      that method or function's matching argument.
///
///
std::optional<NullabilityKind> getNullabilityForParmVarDecl(ParmVarDecl *PVD) {
  return std::nullopt;
}

/// This check deduces the correct nullability of several kinds of pointers in
/// Objective-C code.
///
/// - Objective-C method return type
/// - Function return type
/// - Global const/extern variables (always nonnull, because it's semantically
/// pointless to declare a nil/null global outside of the language itself.)
/// - Arguments are usually nonnull, with some exceptions (see comment on
/// `getNullabilityForParmVarDecl`.)
/// - Obj-C property declarations are nullable if they are marked with the
/// `weak` attribute, or if they are initialized to a nil value in any of the
/// designated initializers for its Obj-C class or its superclasses.
///
/// For methods and functions, we deduce the correct nullability annotation
/// based on examination of all the return statements within that function or
/// method.
///
/// 1. First we match functions ands method decls.
/// 2. When we find a canonical declaration, we gather all the return statements
///   in its redeclarations. (For methods or functions without a prototype, like
///   private Obj-C methods, we collect the return stmts from the canonical decl
///   instead.)
/// 3. Evaluate the nullability of each return statement by examining its return
///    value.
/// 4. We find the weakest nullability value across all the return
///    statements, by comparing them with `clang::hasWeakerNullability`.
///    The weakest nullability wins. This means we will end up with
///    `NullabilityKind::Unassigned` if there's any branch that lacks enough
///    information.
///
/// ---
///  NOTE: Blocks are considered too-complex for the first version of this
///  check, because annotations are carried in their type definitions and
///  therefore canonical decls can conflict with redecls.
///
///  (i.e. If a block is defined with a nonnull argument x, and a function takes
///  the same block except that it marks x as nullable, we'll have a warning
///  from the conflicting nullability checker.)
///
///  If we omit the nullable annotation from the redecleration, we might get the
///  behavior we want. If we omit the annotation from the canonical block decl,
///  we can mark the redecl as nullable, but we get a warning about the
///  canonical block decl missing an annotation. This requires some thought on
///  my part.
///  ---
///
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
    ReturnStatements = returnStatementsForCanonicalDecl<FunctionDecl *>(
        const_cast<FunctionDecl *>(FD));

  } else if (OMD != nullptr) {
    Name = OMD->getQualifiedNameAsString();
    ReturnStatements = returnStatementsForCanonicalDecl<ObjCMethodDecl *>(
        const_cast<ObjCMethodDecl *>(OMD));
  }

  // Resolve the weakest nullability for a decl and its redecls.
  if (FD != nullptr || OMD != nullptr) {
    if (!HasBody && IsCanonical) {
      llvm::errs() << "Found prototype for " << Name << "."
                   << "\n";
    } else {
      std::optional<NullabilityKind> WeakestNullability =
          getWeakestNullabilityForReturnStatements(ReturnStatements,
                                                   Result.Context);
      // If no weakest nullability, there were no return stmts.
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
