---
status: active
audience: public
last-verified: 2026-07-26
---

# Zia Grammar (EBNF)

This document is the formal grammar for the Zia language, in EBNF. It is
descriptive: the hand-written recursive-descent parser in
`src/frontends/zia/Parser_*.cpp` is the implementation of record, and the
[Zia Reference](zia-reference.md) explains semantics. This grammar exists so
tools and AI assistants can reason about Zia syntax without reverse-engineering
the parser.

Notation: `|` alternatives, `[x]` optional, `{x}` zero-or-more, `(x)` grouping,
`"x"` literal terminals. Productions named in `PascalCase`; token-level rules
in `UPPER_CASE`.

---

## Compilation Unit

```ebnf
CompilationUnit  = ModuleDecl { Bind } { TopLevelDecl } ;

ModuleDecl       = "module" IDENT ";" ;

Bind             = "bind" BindTarget [ "as" IDENT ] [ BindList ] ";" ;
BindTarget       = STRING_LIT            (* file bind: "./utils" — .zia is implied *)
                 | QualifiedName ;       (* namespace bind: Zanna.Terminal *)
BindList         = "{" IDENT { "," IDENT } [ "," ] "}" ;

QualifiedName    = IDENT { "." IDENT } ;
```

## Top-Level Declarations

```ebnf
TopLevelDecl     = [ Visibility ] ( FuncDecl
                                  | ClassDecl
                                  | StructDecl
                                  | InterfaceDecl
                                  | EnumDecl
                                  | TypeAliasDecl
                                  | VarDecl ) ;

Visibility       = "expose" | "hide" | "public" | "private" ;

TypeAliasDecl    = "type" IDENT "=" Type ";" ;
```

### Functions

```ebnf
FuncDecl         = [ "async" ] "func" IDENT [ GenericParams ]
                   "(" [ ParamList ] ")" [ "->" Type ] ( Block | "=" Expr ";" ) ;
                   (* `= Expr ;` is the single-expression body form *)
ForeignFuncDecl  = "foreign" "func" IDENT "(" [ ParamList ] ")" [ "->" Type ] [ ";" ] ;

GenericParams    = "[" GenericParam { "," GenericParam } "]" ;
GenericParam     = IDENT [ ":" QualifiedName ] ;  (* single interface constraint, e.g. T: Comparable *)

ParamList        = Param { "," Param } [ "," ] ;
Param            = IDENT ":" [ "..." ] Type [ "=" Expr ] ;
                   (* "..." marks a variadic final parameter; "= Expr" a default *)
```

### Classes, Structs, Interfaces

```ebnf
ClassDecl        = "class" IDENT [ GenericParams ]
                   [ "extends" Type ] [ "implements" TypeList ] ClassBody ;
StructDecl       = "struct" IDENT [ GenericParams ] [ "implements" TypeList ] ClassBody ;
InterfaceDecl    = "interface" IDENT [ GenericParams ] InterfaceBody ;
                   (* interfaces do not support `extends` *)

TypeList         = Type { "," Type } ;

ClassBody        = "{" { MemberDecl } "}" ;
MemberDecl       = { MemberModifier } ( FieldDecl
                                      | MethodDecl
                                      | InitDecl
                                      | DeinitDecl
                                      | PropertyDecl ) ;
MemberModifier   = Visibility | "static" | "override" | "weak" ;

FieldDecl        = [ "var" | "final" | "let" ] IDENT ":" Type [ "=" Expr ] ";"
                 | Type IDENT [ "=" Expr ] ";" ;       (* type-first form *)
                   (* `final`/`let` fields are write-once, assignable only in init *)

MethodDecl       = [ "override" ] [ "async" ] "func" IDENT [ GenericParams ]
                   "(" [ ParamList ] ")" [ "->" Type ] ( Block | "=" Expr ";" ) ;

InitDecl         = "func" "init" "(" [ ParamList ] ")" Block ;
DeinitDecl       = "deinit" Block ;

PropertyDecl     = "property" IDENT ":" Type "{" AccessorList "}" ;
AccessorList     = Accessor { Accessor } ;

InterfaceBody    = "{" { InterfaceMember } "}" ;
InterfaceMember  = "func" IDENT "(" [ ParamList ] ")" [ "->" Type ] ( ";" | Block ) ;
                   (* `;` = abstract; Block = default implementation *)
```

### Enums

```ebnf
EnumDecl         = "enum" IDENT "{" EnumVariant { "," EnumVariant } [ "," ] "}" ;
EnumVariant      = IDENT [ "=" INT_LIT ] ;
```

## Types

```ebnf
Type             = NonOptionalType [ "?" ] ;
NonOptionalType  = QualifiedName [ GenericArgs ]
                 | QualifiedName "[" INT_LIT "]"   (* fixed-size array, e.g. Integer[100] *)
                 | CollectionType
                 | TupleType
                 | FunctionType ;

GenericArgs      = "[" Type { "," Type } "]" ;

CollectionType   = "List" "[" Type "]"
                 | "[" Type "]"                    (* List shorthand: [Integer] == List[Integer] *)
                 | "Map" "[" Type "," Type "]"
                 | "Set" "[" Type "]" ;

TupleType        = "(" Type "," Type { "," Type } ")" ;

FunctionType     = "(" [ Type { "," Type } ] ")" "->" Type ;
```

Built-in scalar type names: `Boolean`, `Integer`, `Number`, `Byte`, `String`,
`Any`, `Never`, `Unit`. Lowercase and historical aliases are accepted
(`int`/`integer`, `bool`/`Bool`, `double`/`float`, `string`, `byte`, `unit`,
`void`, `error`, `any`, `never`).

## Statements

```ebnf
Block            = "{" { Stmt } "}" ;

Stmt             = VarDecl
                 | IfStmt
                 | WhileStmt
                 | ForStmt
                 | MatchStmt
                 | GuardStmt
                 | TryStmt
                 | DeferStmt
                 | ReturnStmt
                 | BreakStmt
                 | ContinueStmt
                 | ThrowStmt
                 | Block
                 | ExprStmt ;

VarDecl          = ( "var" | "final" | "let" ) Binding [ ":" Type ] [ "=" Expr ] ";" ;
Binding          = IDENT
                 | "(" IDENT { "," IDENT } ")" ;   (* tuple destructuring *)

IfStmt           = "if" Condition Block [ "else" ( IfStmt | Block ) ] ;
WhileStmt        = "while" Condition Block ;
Condition        = Expr | "(" Expr ")" ;           (* parentheses optional *)

ForStmt          = CStyleFor | ForInStmt ;
CStyleFor        = "for" "(" [ VarDecl | ExprStmt ] ";" [ Expr ] ";" [ Expr ] ")" Body ;
ForInStmt        = "for" ForBinding "in" Expr Body
                 | "for" "(" ForBinding "in" Expr ")" Body ;
ForBinding       = IDENT [ ":" Type ] [ "," IDENT [ ":" Type ] ] ;
                   (* tuple binding: (index, value) for lists/sets/strings; (key, value) for maps *)
Body             = Block | Stmt ;                  (* single-statement bodies are allowed *)

GuardStmt        = "guard" Condition "else" Stmt ;

MatchStmt        = "match" Expr "{" { MatchArm } "}" ;
MatchArm         = Pattern [ "if" Expr ] "=>" ( Block | Expr ";" ) ;

TryStmt          = "try" Block CatchClauses [ "finally" Block ]
                 | "try" Block "finally" Block ;
CatchClauses     = { "catch" [ "(" IDENT [ ":" IDENT ] ")" ] Block } ;
                   (* typed catches first; catch-all last *)

DeferStmt        = "defer" ( Block | ExprStmt ) ;
ReturnStmt       = "return" [ Expr ] ";" ;
BreakStmt        = "break" ";" ;
ContinueStmt     = "continue" ";" ;
ThrowStmt        = "throw" [ Expr ] ";" ;          (* bare `throw;` rethrows *)
ExprStmt         = Expr ";" ;
```

Assignment is an expression-statement form: `Lvalue AssignOp Expr ";"` with
`AssignOp = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "<<=" | ">>=" | "&=" | "|=" | "^="`.
Compound assignments desugar to `Lvalue = Lvalue op Expr` at parse time, so the
lvalue must be side-effect free. Valid lvalues are identifiers, member accesses,
and index expressions.

## Patterns

```ebnf
Pattern          = OrPattern ;
OrPattern        = PrimaryPattern { "|" PrimaryPattern } ;
PrimaryPattern   = "_"                              (* wildcard *)
                 | INT_LIT ( ".." | "..=" ) INT_LIT (* range pattern: 1..=9 *)
                 | Literal                          (* 1, "x", true, null *)
                 | QualifiedName                    (* enum variant: Color.Red *)
                 | IDENT                            (* binding *)
                 | "(" Pattern { "," Pattern } ")"  (* tuple *)
                 | QualifiedName "(" [ Pattern { "," Pattern } ] ")" ;  (* constructor *)
```

## Expressions

Operator precedence, lowest to highest (all left-associative unless noted). This
table matches the recursive-descent chain in `Parser_Expr.cpp`
(`parseAssignment` → `parseTernary` → `parseRange` → `parseCoalesce` →
`parseLogicalOr` → … → `parseUnary` → `parsePostfix`):

| Level | Operators |
|-------|-----------|
| 1 (lowest) | `=` `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `\|=` `^=` (assignment, right-assoc) |
| 2 | `? :` ternary (right-assoc); `if cond { a } else { b }` if-expression |
| 3 | `..` `..=` (ranges) |
| 4 | `??` (null-coalescing, right-assoc) |
| 5 | `\|\|` / `or` |
| 6 | `&&` / `and` |
| 7 | `\|` (bitwise or) |
| 8 | `^` (bitwise xor) |
| 9 | `&` (bitwise and) |
| 10 | `==` `!=` |
| 11 | `<` `<=` `>` `>=` |
| 12 | `<<` `>>` |
| 13 | `+` `-` |
| 14 | `*` `/` `%` |
| 15 | unary prefix `-` `!` / `not` `~` `&` (function ref), `await` |
| 16 (highest) | postfix: call `(...)`, index `[...]`, member `.`, optional-chain `?.`, force-unwrap `!`, try-propagate `?`, `as` Type, `is` Type |

Note: `is` and `as` are **postfix** type operators (level 16), not binary
comparison operators. Chained relational comparisons (`a < b < c`) and chained
ranges (`a..b..c`) are rejected; parenthesize instead.

```ebnf
Expr             = TernaryExpr ;
TernaryExpr      = NullCoalesce [ "?" Expr ":" Expr ] ;
NullCoalesce     = BinaryExpr { "??" BinaryExpr } ;
BinaryExpr       = (* precedence-climbing over the table above *) ;

UnaryExpr        = ( "-" | "not" | "!" ) UnaryExpr | PostfixExpr ;
PostfixExpr      = PrimaryExpr { Postfix } ;
Postfix          = "(" [ ArgList ] ")"          (* call *)
                 | "[" Expr "]"                 (* index *)
                 | "." IDENT                    (* member *)
                 | "." INT_LIT                  (* tuple index: pair.0 *)
                 | "?." IDENT                   (* optional chain *)
                 | "!"                          (* force unwrap *)
                 | "?"                          (* try-propagation *)
                 | "as" Type                    (* cast *)
                 | "is" Type ;                  (* type test *)
ArgList          = Arg { "," Arg } [ "," ] ;
Arg              = [ IDENT ":" ] Expr ;         (* named argument: name: value *)

PrimaryExpr      = Literal
                 | IDENT
                 | "self" | "super"
                 | "new" QualifiedName "(" [ ArgList ] ")"
                 | "(" Expr ")"
                 | TupleExpr
                 | ListLiteral | MapLiteral | SetLiteral
                 | LambdaExpr
                 | FuncRef
                 | IfExpr
                 | MatchExpr
                 | "await" Expr ;

TupleExpr        = "(" Expr "," Expr { "," Expr } ")" ;
ListLiteral      = "[" [ Expr { "," Expr } [ "," ] ] "]" ;
MapLiteral       = "{" [ Expr ":" Expr { "," Expr ":" Expr } [ "," ] ] "}" ;
SetLiteral       = "{" Expr { "," Expr } [ "," ] "}" ;

LambdaExpr       = "(" [ ParamList ] ")" "=>" ( Expr | Block ) ;
FuncRef          = "&" QualifiedName ;

IfExpr           = "if" Condition Block "else" Block ;   (* both branches, same type *)
MatchExpr        = "match" Expr "{" { MatchArm } "}" ;

RangeExpr        = Expr ".." Expr | Expr "..=" Expr ;    (* chainable: .rev(), .step(n) *)
```

## Lexical Structure

```ebnf
IDENT            = ( LETTER | "_" ) { LETTER | DIGIT | "_" } ;
INT_LIT          = DIGIT { DIGIT | "_" }
                 | "0x" HEXDIGIT { HEXDIGIT | "_" }
                 | "0o" OCTDIGIT { OCTDIGIT | "_" }
                 | "0b" BINDIGIT { BINDIGIT | "_" } ;   (* "_" separators allowed between digits *)
NUM_LIT          = DIGIT { DIGIT | "_" } "." DIGIT { DIGIT | "_" } [ Exponent ]
                 | DIGIT { DIGIT | "_" } Exponent ;      (* e.g. 1e10, 2.5e-3 *)
Exponent         = ( "e" | "E" ) [ "+" | "-" ] DIGIT { DIGIT | "_" } ;
STRING_LIT       = '"' { CHAR | EscapeSeq | Interpolation } '"'
                 | '"""' { ANY } '"""' ;                 (* multi-line *)
Interpolation    = "${" Expr "}" ;
EscapeSeq        = "\n" | "\t" | "\r" | "\\" | "\"" | "\'" | "\0"
                 | "\x" HEXDIGIT HEXDIGIT | "\u" HEXDIGIT HEXDIGIT HEXDIGIT HEXDIGIT ;
Literal          = INT_LIT | NUM_LIT | STRING_LIT | "true" | "false" | "null" ;

LineComment      = "//" { ANY-except-newline } ;
BlockComment     = "/*" { ANY } "*/" ;                   (* nestable *)
DocComment       = "///" { ANY-except-newline } ;
```

### Reserved Words

```text
and as async await bind break catch class continue defer deinit else enum
export expose extends false final finally for foreign func guard
hide if implements in interface is let match module namespace new not null
or override private property public return self static struct super throw
true try type var weak while
```

---

## Cross-References

- Semantics and worked examples: [Zia Reference](zia-reference.md)
- Tutorial: [Zia Getting Started](../tutorials/zia-tutorial.md)
- BASIC equivalent: [BASIC Grammar Notes](basic-grammar.md)
- Parser implementation: `src/frontends/zia/Parser_Decl.cpp`,
  `Parser_Stmt.cpp`, `Parser_Expr.cpp`, `Parser_Expr_Primary.cpp`,
  `Parser_Expr_Pattern.cpp`, `Parser_Type.cpp`

If this grammar and the parser disagree, the parser wins — please file the
discrepancy so this document can be corrected.
