// RUN: %check_clang_tidy %s objc-nullability-annotator %t

// This annotator performs static analysis and adds nullability annotations to 
// returns types, property types, and arguments.

// CHECK-MESSAGES: :[[@LINE-1]]:6: warning: function 'f' is insufficiently awesome [objc-nullability-annotator]


@interface NSObject
+ (instancetype)alloc;
- (instancetype)init;
- (instancetype)self;
@end

@class NSString;

// Some helper definitions of null.
#define nil (id)0
#define NULL (void *)0

// A freestanding function that returns a string literal.
NSString *_Nonnull annotatedFreestandingReturnsStringLiteral() {
  return @"I am a string.";
}

// A freestanding function that returns a `nil` literal.
NSString *_Nullable annotatedReturnsNilLiteral() {
  return nil;
}

// CHECK: NSString *_Nonnull unannotatedFunctionReturnsStringLiteral() {
NSString * unannotatedFunctionReturnsStringLiteral() {
  return @"I am a string, too.";
}

// CHECK: NSString *_Nullable unannotatedFunctionReturnsNilLiteral() {
NSString * unannotatedFunctionReturnsNilLiteral() {
  return nil;
}

@interface ClassWithoutAnnotations: NSObject

// Single-branch methods, without arguments
- (instancetype)init;
+ (id)init;

// CHECK: - (void)noReturn;
- (void)noReturn;

// CHECK: - (NSObject *_Nullable)returnsNULLLiteralObjectPointer;
- (NSObject *)returnsNULLLiteralObjectPointer;
// CHECK: - (NSObject *_Nullable)returnsNilLiteralObjectPointer;
- (NSObject *)returnsNilLiteralObjectPointer;
// CHECK: - (NSObject *_Nullable)returnsNullPtrObjectPointer;
- (NSObject *)returnsNullPtrObjectPointer;

// CHECK: - (id _Nullable)returnsNilLiteralIDType;
- (id)returnsNilLiteralIDType;
// CHECK: - (id _Nullable)returnsNullPtrIDType;
- (id)returnsNullPtrIDType;
// CHECK: - (id _Nullable)returnsNULLLiteralIDType;
- (id)returnsNULLLiteralIDType;
// CHECK: - (instancetype _Nullable)returnsNilLiteralInstanceType;
- (instancetype)returnsNilLiteralInstanceType;
// CHECK: - (id _Nullable)returnsNilLiteralInstanceType;
- (id)returnsNilLiteralInstanceType;
// CHECK: - (NSString *_Nonnull)returnsLiteralString;
- (NSString *)returnsLiteralString;

// Class methods, too.

// CHECK: + (instancetype _Nullable)returnsNilLiteralInstanceTypeClassMethod;
+ (instancetype)returnsNilLiteralInstanceTypeClassMethod;
// CHECK: + (id _Nullable)returnsNilLiteralInstanceTypeClassMethod;
+ (id)returnsNilLiteralInstanceTypeClassMethod;
// + (NSString *_Nonnull)returnsStringLiteralClassMethod;
+ (NSString *)returnsStringLiteralClassMethod;

// Branching logic
// CHECK: - (NSString * _Nullable)maybeNilMaybeNot;
- (NSString *)maybeNilMaybeNot;
@end

// Covers cases where annotated arguments or annotated methods contribute to
// the annotation correctness of another method, property, or argument in a 
// the class.
@interface ClassWithMissingAnnotations: NSObject

// Return types and arguments. Simple case where an argument is treated as nullable.
// CHECK: - (NSString *_Nullable)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument;
- (NSString *)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument;
// CHECK: - (id _Nullable)returnsNullableIDTypeArgumentDirectly:(id _Nullable)argument;
- (id)returnsNullableIDTypeArgumentDirectly:(id _Nullable)argument;
// CHECK: - (NSString *_Nonnull)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument;
- (NSString *)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument;
// CHECK: - (id _Nonnull)returnsNonnullIDTypeArgumentDirectly:(id _Nonnull)argument;
- (id)returnsNonnullIDTypeArgumentDirectly:(id _Nonnull)argument;

// CHECK: - (NSString *)callsNullableFunction;
- (NSString *)callsNullableFunction;
// CHECK: - (NSString *)callsNonnullFunction;
- (NSString *)callsNonnullFunction;
// CHECK: - (NSString *_Nonnull)annotatedNonnull; 
- (NSString *_Nonnull)annotatedNonnull;
// CHECK: - (NSString *_Nullable)annotatedNullable;
- (NSString *_Nullable)annotatedNullable;
// CHECK: - (nonnull NSString *)annotatedNonnullWithSugar;
- (nonnull NSString *)annotatedNonnullWithSugar;
// CHECK: - (nullable NSString *)annotatedNullableWithSugar;
- (nullable NSString *)annotatedNullableWithSugar;

@end 

id _Nullable returnsEmpty(int kind) {
  switch (kind) {
    case 0:
      // NULL
      return 	(void *)0;
    case 1: 
      // nil
      return (id)0;
    case 2: 
      // Nil (capital `N`)
      return (Class)0;
    default:
      return 0;
  }
}