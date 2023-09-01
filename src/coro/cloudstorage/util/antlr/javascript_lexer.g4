lexer grammar javascript_lexer;

options { superClass=JavaScriptLexerBase; }

@header {#include "antlr/javascript_lexer_base.h"}

SemiColon: ';';
Colon: ':';
Comma: ',';
Assign: '=';
OpenParenthesis: '(';
CloseParenthesis: ')';
OpenBrace: '{';
CloseBrace: '}';
OpenBracket: '[';
CloseBracket: ']';
QuestionMark: '?';
Dot: '.';

Increment: '++';
Decrement: '--';
Plus: '+';
Minus: '-';
Modulus: '%';
Multiply: '*';
Divide: '/';
PlusAssign: '+=';
MinusAssign: '-=';
ModulusAssign: '%=';
MultiplyAssign: '*=';
DivideAssign: '/=';
StrictEquals: '===';
NotStrictEquals: '!==';
Equals: '==';
NotEquals: '!=';
GreaterEqual: '>=';
Greater: '>';
LessEqual: '<=';
Less: '<';
And: '&&';
Or: '||';
BitOr: '|';
BitShiftLeft: '<<';
BitShiftRight: '>>';
BitUnsignedRightShift: '>>>';
BitXor: '^';

Throw: 'throw';
Void: 'void';
New: 'new';
Function: 'function';
Default: 'default';
Return: 'return';
Continue: 'continue';
Break: 'break';
Try: 'try';
Catch: 'catch';
Case: 'case';
For: 'for';
Switch: 'switch';
If: 'if';
NotANumber: 'NaN';

Integer: [0-9]+;
Real: [0-9][0-9eE.]*;
String: '"' ~["]* '"';
Var: 'var';
Identifier: [A-Za-z]+[0-9]*;
RegularExpressionLiteral: '/' RegularExpressionFirstChar RegularExpressionChar* {this->IsRegexPossible()}? '/' IdentifierPart*;
WS: [ \n\t\r] -> skip;

fragment HexDigit
    : [_0-9a-fA-F]
    ;

fragment HexEscapeSequence
    : 'x' HexDigit HexDigit
    ;

fragment UnicodeEscapeSequence
    : 'u' HexDigit HexDigit HexDigit HexDigit
    | 'u' '{' HexDigit HexDigit+ '}'
    ;

fragment IdentifierPart
    : IdentifierStart
    | [\p{Mn}]
    | [\p{Nd}]
    | [\p{Pc}]
    | '\u200C'
    | '\u200D'
    ;

fragment IdentifierStart
    : [\p{L}]
    | [$_]
    | '\\' UnicodeEscapeSequence
    ;

fragment RegularExpressionFirstChar
    : ~[*\r\n\u2028\u2029\\/[]
    | RegularExpressionBackslashSequence
    | '[' RegularExpressionClassChar* ']'
    ;

fragment RegularExpressionChar
    : ~[\r\n\u2028\u2029\\/[]
    | RegularExpressionBackslashSequence
    | '[' RegularExpressionClassChar* ']'
    ;

fragment RegularExpressionClassChar
    : ~[\r\n\u2028\u2029\]\\]
    | RegularExpressionBackslashSequence
    ;

fragment RegularExpressionBackslashSequence
    : '\\' ~[\r\n\u2028\u2029]
    ;
