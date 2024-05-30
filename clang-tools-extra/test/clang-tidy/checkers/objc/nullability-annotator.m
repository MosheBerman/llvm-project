// RUN: %check_clang_tidy %s objc-nullability-annotator %t

// This annotator performs static analysis and adds nullability annotations to 
// returns types, property types, and arguments.

// CHECK-MESSAGES: :[[@LINE-1]]:6: warning: function 'f' is insufficiently awesome [objc-nullability-annotator]

@interface NSObject
+ (nonnull instancetype)alloc;
- (nonnull instancetype)init;
- (nonnull instancetype)self;
@end

@class NSString;

// Some helper definitions of null.
#define nil (id)0
#define NULL (void *)0

// A freestanding function that returns a string literal.
NSString *_Nonnull annotatedFreestandingReturnsStringLiteral(void) {
  return @"I am a string.";
}

// A freestanding function that returns a `nil` literal.
NSString *_Nullable annotatedReturnsNilLiteral(void) {
  return nil;
}

// A freestanding function that returns a `NULL` literal.
NSString *_Nullable annotatedReturnsNULLLiteral(void) {
  return NULL;
}

// CHECK: NSString *_Nonnull unannotatedFunctionReturnsStringLiteral() {
NSString * unannotatedFunctionReturnsStringLiteral(void) {
  return @"I am a string, too.";
}

// CHECK: NSString *_Nullable unannotatedFunctionReturnsNilLiteral() {
NSString * unannotatedFunctionReturnsNilLiteral(void) {
  return nil;
}

@interface ClassWithoutAnnotations: NSObject

// Single-branch methods, without arguments
- (instancetype)init;
+ (id)init;

// CHECK: - (void)noReturn;
- (void)noReturn;
// CHECK: - (void)voidReturn;
- (void)voidReturn;

// CHECK: - (NSObject *_Nullable)returnsNULLLiteralObjectPointer;
- (NSObject *)returnsNULLLiteralObjectPointer;
// CHECK: - (NSObject *_Nullable)returnsNilLiteralObjectPointer;
- (NSObject *)returnsNilLiteralObjectPointer;
// CHECK: - (id _Nullable)returnsNilLiteralIdType;
- (id)returnsNilLiteralIdType;
// CHECK: - (id _Nullable)returnsNULLLiteralIdType;
- (id)returnsNULLLiteralIdType;
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

@implementation ClassWithoutAnnotations

// Single-branch methods, without arguments
- (instancetype)init {
return [super init];
}

+ (id)init {
  return [super init];
}

// CHECK: - (void)noReturn;
- (void)noReturn{}

- (void)voidReturn{
  return;
}

// CHECK: - (NSObject *_Nullable)returnsNULLLiteralObjectPointer;
- (NSObject *)returnsNULLLiteralObjectPointer {
  return NULL;
}
// CHECK: - (NSObject *_Nullable)returnsNilLiteralObjectPointer;
- (NSObject *)returnsNilLiteralObjectPointer {
  return nil;
}

// CHECK: - (id _Nullable)returnsNilLiteralIdType;
- (id)returnsNilLiteralIdType {
  return nil;
}
// CHECK: - (id _Nullable)returnsNULLLiteralIdType;
- (id)returnsNULLLiteralIdType {
  return NULL;
}

// CHECK: - (instancetype _Nullable)returnsNilLiteralInstanceType;
- (instancetype)returnsNilLiteralInstanceType {
  return nil;
}

// CHECK: - (NSString *_Nonnull)returnsLiteralString;
- (NSString *)returnsLiteralString {
  return nil;
}

// Class methods, too.

// CHECK: + (instancetype _Nullable)returnsNilLiteralInstanceTypeClassMethod;
+ (instancetype)returnsNilLiteralInstanceTypeClassMethod {
  return nil;
}
// CHECK: + (id _Nullable)returnsNilLiteralInstanceTypeClassMethod;
+ (id)returnsNilLiteralIdTypeClassMethod {
  return nil;
}
// + (NSString *_Nonnull)returnsStringLiteralClassMethod;
+ (NSString *)returnsStringLiteralClassMethod {
  return @"I am a string literal";
}

// Branching logic
// CHECK: - (NSString * _Nullable)maybeNilMaybeNot;
- (NSString *)maybeNilMaybeNot {
  if (1) {
    return @"I am a string literal.";
  } 
  return nil;
}
@end

// Covers cases where annotated arguments or annotated methods contribute to
// the annotation correctness of another method, property, or argument in a 
// the class.
@interface ClassWithMissingAnnotations: NSObject

// Return types and arguments. Simple case where an argument is treated as nullable.
// CHECK:- (NSString *_Null_unspecified)returnsNullUnspecifiedStringArgumentDirectly:(NSString * _Null_unspecified)argument;
- (NSString *)returnsNullUnspecifiedStringArgumentDirectly:(NSString * _Null_unspecified)argument;
// CHECK: - (id _Null_unspecified)returnsNullUnspecifiedIdTypeArgumentDirectly:(id _Null_unspecified)argument;
- (id)returnsNullUnspecifiedIdTypeArgumentDirectly:(id _Null_unspecified)argument;
// CHECK: - (NSString *_Nullable)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument;
- (NSString *)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument;
// CHECK: - (id _Nullable)returnsNullableIdTypeArgumentDirectly:(id _Nullable)argument;
- (id)returnsNullableIdTypeArgumentDirectly:(id _Nullable)argument;
// CHECK: - (NSString *_Nonnull)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument;
- (NSString *)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument;
// CHECK: - (id _Nonnull)returnsNonnullIdTypeArgumentDirectly:(id _Nonnull)argument;
- (id)returnsNonnullIdTypeArgumentDirectly:(id _Nonnull)argument;

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


// Covers cases where annotated arguments or annotated methods contribute to
// the annotation correctness of another method, property, or argument in a 
// the class.
@implementation ClassWithMissingAnnotations

// Return types and arguments. Simple case where an argument is treated as nullable.
// CHECK:- (NSString *_Null_unspecified)returnsNullUnspecifiedStringArgumentDirectly:(NSString * _Null_unspecified)argument;
- (NSString *)returnsNullUnspecifiedStringArgumentDirectly:(NSString * _Null_unspecified)argument {
  return argument;
}

// CHECK: - (id _Null_unspecified)returnsNullUnspecifiedIdTypeArgumentDirectly:(id _Null_unspecified)argument;
- (id)returnsNullUnspecifiedIdTypeArgumentDirectly:(id _Null_unspecified)argument {
  return argument;
}

// Return types and arguments. Simple case where an argument is treated as nullable.
// CHECK: - (NSString *_Nullable)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument;
- (NSString *)returnsNullableStringArgumentDirectly:(NSString * _Nullable)argument {
  return argument;
}

// CHECK: - (id _Nullable)returnsNullableIdTypeArgumentDirectly:(id _Nullable)argument;
- (id)returnsNullableIdTypeArgumentDirectly:(id _Nullable)argument {
  return argument;
}
// CHECK: - (NSString *_Nonnull)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument;
- (NSString *)returnsNonnullStringArgumentDirectly:(NSString * _Nonnull)argument {
  return argument;
}

// CHECK: - (id _Nonnull)returnsNonnullIdTypeArgumentDirectly:(id _Nonnull)argument;
- (id)returnsNonnullIdTypeArgumentDirectly:(id _Nonnull)argument {
  return argument;
}

// CHECK: - (NSString *)callsNullableFunction;
- (NSString *)callsNullableFunction {
  return annotatedReturnsNilLiteral();
}
// CHECK: - (NSString *)callsNonnullFunction;
- (NSString *)callsNonnullFunction {
  return annotatedFreestandingReturnsStringLiteral();
}

// CHECK: - (NSString *_Nonnull)annotatedNonnull; 
- (NSString *_Nonnull)annotatedNonnull {
  return @"I'm also a string.";
}

// CHECK: - (NSString *_Nullable)annotatedNullable;
- (NSString *_Nullable)annotatedNullable {
  return nil;
}

// CHECK: - (nonnull NSString *)annotatedNonnullWithSugar;
- (nonnull NSString *)annotatedNonnullWithSugar {
  return @"I am string with a sweet tooth. I like syntax-sugar.";
}

// CHECK: - (nullable NSString *)annotatedNullableWithSugar;
- (nullable NSString *)annotatedNullableWithSugar {
  return nil;
}

@end 


id returnsEmpty(int Kind) {
  switch (Kind) {
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
