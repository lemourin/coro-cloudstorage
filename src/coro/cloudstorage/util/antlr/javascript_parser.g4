parser grammar javascript_parser;

options {
    tokenVocab=javascript_lexer;
}

root
    : block ';'? EOF
    ;

statement
    : declaration ';'?  #DeclarationStatement
    | expression ( ',' expression )* ';'?  #ExpressionStatement
    | block  #BlockStatement
    | forLoop  #ForLoopStatement
    | switchBlock  #SwitchBlockStatement
    | tryCatchBlock  #TryCatchBlockStatement
    | Break ';'?  #BreakStatement
    | Continue ';'?  #ContinueStatement
    | Return expression ';'?  #ReturnStatement
    | ';'  #EmptyStatement
    | If '(' expression ')' statement  #IfStatement
    | Throw expression ';'?  #ThrowStatement
    ;

tryCatchBlock
    : Try block Catch '(' Identifier ')' block
    ;

block
    : '{' statement* '}'
    ;

forLoop
    : For '(' (init_decl=declaration|init_expr=expression)? ';' cond=expression? ';' step=expression? ')' statement
    ;

switchBlock
    : Switch '(' expression ')' ( '{' caseBlock* '}' | caseBlock )
    ;

caseBlock
    : 'case' constant ':' statement*
    | 'default' ':' statement*
    ;

oneDeclaration
    : Identifier '=' expression
    ;

declaration
    : 'var' oneDeclaration ( ',' oneDeclaration )*
    ;

expression
    : constant  #ConstantExpression
    | Identifier  #IdentifierExpression
    | New Identifier '(' (expression (',' expression)* )? ')'  #NewExpression
    | Void expression  #VoidExpression
    | '(' expression (',' expression)* ')' #ParenthesisExpression
    | '[' ( expression ( ',' expression )* )? ']'  #ArrayExpression
    | expression '[' expression ']'  #SubscriptExpression
    | 'function' '(' (Identifier (',' Identifier)* )? ')' block  #FunctionExpression
    | expression '.' Identifier '(' ( expression ( ',' expression )* )? ')'  #MethodExpression
    | expression '.' Identifier  #MemberExpression
    | expression '(' ( expression ( ',' expression )* )? ')'  #CallExpression
    | op=('++'|'--') expression  #PreIncrementExpression
    | expression op=('++'|'--')  #PostIncrementExpression
    | expression op=('+='|'-='|'*='|'/='|'%=') expression  #MutateExpression
    | '-' expression  #UnaryMinusExpression
    | expression '/' expression  #DivExpression
    | expression '%' expression  #ModExpression
    | expression '*' expression  #MulExpression
    | expression '-' expression  #SubExpression
    | expression '+' expression  #SumExpression
    | expression '=' expression  #AssignmentExpression
    | expression '>=' expression  #GreaterOrEqualExpression
    | expression '>' expression  #GreaterExpression
    | expression '<=' expression  #LessOrEqualExpression
    | expression '<' expression  #LessExpression
    | expression '<<' expression  #BitShiftLeftExpression
    | expression '|' expression  #BitOrExpression
    | expression '^' expression  #BitXorExpression
    | expression '?' expression ':' expression  #TernaryOperatorExpression
    | expression '===' expression  #StrictEqualExpression
    | expression '!==' expression  #NotStrictEqualExpression
    | expression '==' expression  #EqualExpression
    | expression '!=' expression  #NotEqualExpression
    | expression '&&' expression  #AndExpression
    | expression '||' expression  #OrExpression
    ;

object
    : '{' '}'
    ;

constant
    : Real
    | Integer
    | String
    | RegularExpressionLiteral
    | NotANumber
    | object
    ;
