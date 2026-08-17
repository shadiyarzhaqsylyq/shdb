package main

import "core:fmt"
import "core:strings"

// ---------------------------------------------------------------------------
// 1. TOKEN DEFINITIONS
// ---------------------------------------------------------------------------

Token_Type :: enum {
	EOF,
	ILLEGAL,

	// Keywords & Identifiers
	IDENTIFIER,
	KEYWORD_CREATE,
	KEYWORD_TABLE,
	KEYWORD_INT,
	KEYWORD_VARCHAR,
	KEYWORD_PRIMARY,
	KEYWORD_KEY,

	// Delimiters & Symbols
	LPAREN,    // (
	RPAREN,    // )
	COMMA,     // ,
	SEMICOLON, // ;
}

Token :: struct {
	type: Token_Type,
	text: string,
}

// ---------------------------------------------------------------------------
// 2. LEXER
// ---------------------------------------------------------------------------

Lexer :: struct {
	input:         string,
	position:      int, // Current byte index
	read_position: int, // Next byte index
	ch:            byte, // Current character
}

lexer_init :: proc(input: string) -> Lexer {
	l := Lexer{input = input}
	lexer_read_char(&l)
	return l
}

lexer_read_char :: proc(l: ^Lexer) {
	if l.read_position >= len(l.input) {
		l.ch = 0 // EOF
	} else {
		l.ch = l.input[l.read_position]
	}
	l.position = l.read_position
	l.read_position += 1
}

lexer_skip_whitespace :: proc(l: ^Lexer) {
	for l.ch == ' ' || l.ch == '\t' || l.ch == '\n' || l.ch == '\r' {
		lexer_read_char(l)
	}
}

is_letter :: proc(ch: byte) -> bool {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'
}

is_alphanumeric :: proc(ch: byte) -> bool {
	return is_letter(ch) || (ch >= '0' && ch <= '9')
}

lookup_keyword :: proc(ident: string) -> Token_Type {
	upper := strings.to_upper(ident, context.temp_allocator)
	switch upper {
	case "CREATE":  return .KEYWORD_CREATE
	case "TABLE":   return .KEYWORD_TABLE
	case "INT":     return .KEYWORD_INT
	case "VARCHAR": return .KEYWORD_VARCHAR
	case "PRIMARY": return .KEYWORD_PRIMARY
	case "KEY":     return .KEYWORD_KEY
	case:           return .IDENTIFIER
	}
}

lexer_next_token :: proc(l: ^Lexer) -> Token {
	lexer_skip_whitespace(l)

	tok: Token

	switch l.ch {
	case '(':
		tok = Token{.LPAREN, "("}
		lexer_read_char(l)
	case ')':
		tok = Token{.RPAREN, ")"}
		lexer_read_char(l)
	case ',':
		tok = Token{.COMMA, ","}
		lexer_read_char(l)
	case ';':
		tok = Token{.SEMICOLON, ";"}
		lexer_read_char(l)
	case 0:
		tok = Token{.EOF, ""}
	case:
		if is_letter(l.ch) {
			start := l.position
			for is_alphanumeric(l.ch) {
				lexer_read_char(l)
			}
			ident := l.input[start:l.position]
			tok.type = lookup_keyword(ident)
			tok.text = ident
			return tok
		} else {
			tok = Token{.ILLEGAL, string([]byte{l.ch})}
			lexer_read_char(l)
		}
	}

	return tok
}

// ---------------------------------------------------------------------------
// 3. ABSTRACT SYNTAX TREE (AST)
// ---------------------------------------------------------------------------

Column_Def :: struct {
	name:           string,
	data_type:      string,
	is_primary_key: bool,
}

Create_Table_AST :: struct {
	table_name: string,
	columns:    [dynamic]Column_Def,
}

// ---------------------------------------------------------------------------
// 4. RECURSIVE DESCENT PARSER
// ---------------------------------------------------------------------------

Parser :: struct {
	lexer:      Lexer,
	cur_token:  Token,
	peek_token: Token,
	errors:     [dynamic]string,
}

parser_init :: proc(l: ^Lexer) -> Parser {
	p := Parser{
		lexer = l^,
		errors = make([dynamic]string),
	}
	// Initialize cur_token and peek_token
	parser_next_token(&p)
	parser_next_token(&p)
	return p
}

parser_next_token :: proc(p: ^Parser) {
	p.cur_token = p.peek_token
	p.peek_token = lexer_next_token(&p.lexer)
}

parser_expect :: proc(p: ^Parser, t: Token_Type) -> bool {
	if p.cur_token.type == t {
		parser_next_token(p)
		return true
	}
	err := fmt.tprintf("Expected token %v, got '%s' (%v)", t, p.cur_token.text, p.cur_token.type)
	append(&p.errors, err)
	return false
}

// Parses identifiers, including when reserved keywords like "TABLE" are used as identifiers
parse_name :: proc(p: ^Parser) -> (string, bool) {
	if p.cur_token.type == .IDENTIFIER || p.cur_token.type == .KEYWORD_TABLE {
		name := p.cur_token.text
		parser_next_token(p)
		return name, true
	}
	err := fmt.tprintf("Expected identifier, got '%s'", p.cur_token.text)
	append(&p.errors, err)
	return "", false
}

parse_column_def :: proc(p: ^Parser) -> (Column_Def, bool) {
	col: Column_Def

	// 1. Column Name
	name, ok_name := parse_name(p)
	if !ok_name do return col, false
	col.name = name

	// 2. Data Type
	if p.cur_token.type != .KEYWORD_INT && p.cur_token.type != .KEYWORD_VARCHAR && p.cur_token.type != .IDENTIFIER {
		err := fmt.tprintf("Expected data type for column '%s', got '%s'", col.name, p.cur_token.text)
		append(&p.errors, err)
		return col, false
	}
	col.data_type = p.cur_token.text
	parser_next_token(p)

	// 3. Optional Constraint: PRIMARY KEY
	if p.cur_token.type == .KEYWORD_PRIMARY {
		parser_next_token(p)
		if !parser_expect(p, .KEYWORD_KEY) {
			return col, false
		}
		col.is_primary_key = true
	}

	return col, true
}

parse_create_table :: proc(p: ^Parser) -> (Create_Table_AST, bool) {
	ast: Create_Table_AST
	ast.columns = make([dynamic]Column_Def)

	// CREATE
	if !parser_expect(p, .KEYWORD_CREATE) do return ast, false

	// TABLE
	if !parser_expect(p, .KEYWORD_TABLE) do return ast, false

	// Table Name (Handles "table" as a name contextually)
	table_name, ok_tbl := parse_name(p)
	if !ok_tbl do return ast, false
	ast.table_name = table_name

	// '('
	if !parser_expect(p, .LPAREN) do return ast, false

	// Columns List
	for p.cur_token.type != .RPAREN && p.cur_token.type != .EOF {
		col, ok_col := parse_column_def(p)
		if !ok_col do return ast, false
		append(&ast.columns, col)

		if p.cur_token.type == .COMMA {
			parser_next_token(p) // consume ','
		} else if p.cur_token.type != .RPAREN {
			err := fmt.tprintf("Expected ',' or ')', got '%s'", p.cur_token.text)
			append(&p.errors, err)
			return ast, false
		}
	}

	// ')'
	if !parser_expect(p, .RPAREN) do return ast, false

	// Optional ';'
	if p.cur_token.type == .SEMICOLON {
		parser_next_token(p)
	}

	return ast, true
}

// ---------------------------------------------------------------------------
// 5. MAIN DEMO
// ---------------------------------------------------------------------------

main :: proc() {
	sql_query := "CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);"

	fmt.println("Input SQL:")
	fmt.println(sql_query)
	fmt.println("\n--------------------------------------------------")

	// Initialize Lexer & Parser
	lexer := lexer_init(sql_query)
	parser := parser_init(&lexer)

	// Parse Query
	ast, ok := parse_create_table(&parser)

	if !ok || len(parser.errors) > 0 {
		fmt.println("\nParser Errors:")
		for err in parser.errors {
			fmt.printf(" - %s\n", err)
		}
		return
	}

	// Output AST Result
	fmt.printf("\nSuccessfully Parsed Statement!\n")
	fmt.printf("Table Name: %s\n", ast.table_name)
	fmt.println("Columns:")
	for col in ast.columns {
		pk_str := col.is_primary_key ? " [PRIMARY KEY]" : ""
		fmt.printf("  - %-10s %-10s%s\n", col.name, col.data_type, pk_str)
	}
}
/*
output:
Input SQL:
CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);

--------------------------------------------------

Successfully Parsed Statement!
Table Name: table
Columns:
  - id         INT        [PRIMARY KEY]
  - name       VARCHAR   
  - did        VARCHAR   
  - dep        VARCHAR   
  - salary     INT       
  - city       VARCHAR   


*/
