#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ========================================================================= *
 * 1. LEXER (TOKENIZER)
 * ========================================================================= */

typedef enum {
    TOKEN_EOF = 0,
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_INT,
    TOKEN_VARCHAR,
    TOKEN_PRIMARY,
    TOKEN_KEY,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_LPAREN,     // (
    TOKEN_RPAREN,     // )
    TOKEN_COMMA,      // ,
    TOKEN_SEMICOLON,  // ;
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int length;
} Token;

typedef struct {
    const char *source;
    int cursor;
} Lexer;

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->cursor = 0;
}

static char lexer_peek(Lexer *lexer) {
    return lexer->source[lexer->cursor];
}

static char lexer_advance(Lexer *lexer) {
    if (lexer->source[lexer->cursor] == '\0') return '\0';
    return lexer->source[lexer->cursor++];
}

static void lexer_skip_whitespace(Lexer *lexer) {
    while (isspace((unsigned char)lexer_peek(lexer))) {
        lexer_advance(lexer);
    }
}

static bool str_equals_case_insensitive(const char *a, const char *b, int len) {
    for (int i = 0; i < len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return b[len] == '\0';
}

static TokenType check_keyword_or_ident(const char *start, int len) {
    if (str_equals_case_insensitive(start, "CREATE", len))  return TOKEN_CREATE;
    if (str_equals_case_insensitive(start, "TABLE", len))   return TOKEN_TABLE;
    if (str_equals_case_insensitive(start, "INT", len))     return TOKEN_INT;
    if (str_equals_case_insensitive(start, "VARCHAR", len)) return TOKEN_VARCHAR;
    if (str_equals_case_insensitive(start, "PRIMARY", len)) return TOKEN_PRIMARY;
    if (str_equals_case_insensitive(start, "KEY", len))     return TOKEN_KEY;
    return TOKEN_IDENTIFIER;
}

Token lexer_next_token(Lexer *lexer) {
    lexer_skip_whitespace(lexer);

    const char *start = &lexer->source[lexer->cursor];
    char c = lexer_advance(lexer);

    if (c == '\0') {
        return (Token){TOKEN_EOF, start, 0};
    }

    switch (c) {
        case '(': return (Token){TOKEN_LPAREN, start, 1};
        case ')': return (Token){TOKEN_RPAREN, start, 1};
        case ',': return (Token){TOKEN_COMMA, start, 1};
        case ';': return (Token){TOKEN_SEMICOLON, start, 1};
    }

    // Number literals (e.g. for VARCHAR(255))
    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)lexer_peek(lexer))) {
            lexer_advance(lexer);
        }
        return (Token){TOKEN_NUMBER, start, (int)(&lexer->source[lexer->cursor] - start)};
    }

    // Identifiers & Keywords
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)lexer_peek(lexer)) || lexer_peek(lexer) == '_') {
            lexer_advance(lexer);
        }
        int len = (int)(&lexer->source[lexer->cursor] - start);
        TokenType type = check_keyword_or_ident(start, len);
        return (Token){type, start, len};
    }

    return (Token){TOKEN_ERROR, start, 1};
}

/* ========================================================================= *
 * 2. ABSTRACT SYNTAX TREE (AST) DEFINITIONS
 * ========================================================================= */

typedef enum {
    DATA_TYPE_INT,
    DATA_TYPE_VARCHAR
} DataType;

typedef struct ColumnDef {
    char *name;
    DataType type;
    int varchar_length;       // Optional length (e.g. 255 if unspecified)
    bool is_primary_key;
    struct ColumnDef *next;   // Linked list of column definitions
} ColumnDef;

typedef struct {
    char *table_name;
    ColumnDef *columns;
    int column_count;
} CreateTableStmt;

/* Helper to duplicate token strings */
static char *token_to_string(Token token) {
    char *str = (char *)malloc(token.length + 1);
    memcpy(str, token.start, token.length);
    str[token.length] = '\0';
    return str;
}

/* ========================================================================= *
 * 3. RECURSIVE DESCENT PARSER
 * ========================================================================= */

typedef struct {
    Lexer *lexer;
    Token current;
    Token peek;
} Parser;

static void parser_advance(Parser *parser) {
    parser->current = parser->peek;
    parser->peek = lexer_next_token(parser->lexer);
}

void parser_init(Parser *parser, Lexer *lexer) {
    parser->lexer = lexer;
    parser->current = lexer_next_token(lexer);
    parser->peek = lexer_next_token(lexer);
}

static bool parser_match(Parser *parser, TokenType type) {
    if (parser->current.type == type) {
        parser_advance(parser);
        return true;
    }
    return false;
}

static bool parser_expect(Parser *parser, TokenType type, const char *err_msg) {
    if (parser->current.type == type) {
        parser_advance(parser);
        return true;
    }
    fprintf(stderr, "Syntax Error: %s. Got '%.*s'\n", 
            err_msg, parser->current.length, parser->current.start);
    return false;
}

/* Parses an identifier. Allows keywords to be used as table/col names */
static char *parse_identifier(Parser *parser) {
    if (parser->current.type == TOKEN_IDENTIFIER || 
        parser->current.type == TOKEN_TABLE      ||
        parser->current.type == TOKEN_KEY) {
        
        char *id = token_to_string(parser->current);
        parser_advance(parser);
        return id;
    }
    fprintf(stderr, "Syntax Error: Expected identifier, got '%.*s'\n",
            parser->current.length, parser->current.start);
    return NULL;
}

static ColumnDef *parse_column_def(Parser *parser) {
    char *col_name = parse_identifier(parser);
    if (!col_name) return NULL;

    DataType type;
    int varchar_len = 255; // Default length

    if (parser_match(parser, TOKEN_INT)) {
        type = DATA_TYPE_INT;
    } else if (parser_match(parser, TOKEN_VARCHAR)) {
        type = DATA_TYPE_VARCHAR;
        // Handle optional VARCHAR(len)
        if (parser_match(parser, TOKEN_LPAREN)) {
            if (parser->current.type == TOKEN_NUMBER) {
                varchar_len = atoi(parser->current.start);
                parser_advance(parser);
            } else {
                fprintf(stderr, "Syntax Error: Expected number inside VARCHAR(...)\n");
                free(col_name);
                return NULL;
            }
            if (!parser_expect(parser, TOKEN_RPAREN, "Expected ')' after VARCHAR length")) {
                free(col_name);
                return NULL;
            }
        }
    } else {
        fprintf(stderr, "Syntax Error: Expected data type for column '%s'\n", col_name);
        free(col_name);
        return NULL;
    }

    bool is_primary_key = false;
    if (parser_match(parser, TOKEN_PRIMARY)) {
        if (!parser_expect(parser, TOKEN_KEY, "Expected 'KEY' after 'PRIMARY'")) {
            free(col_name);
            return NULL;
        }
        is_primary_key = true;
    }

    ColumnDef *col = (ColumnDef *)malloc(sizeof(ColumnDef));
    col->name = col_name;
    col->type = type;
    col->varchar_length = varchar_len;
    col->is_primary_key = is_primary_key;
    col->next = NULL;

    return col;
}

CreateTableStmt *parse_create_table(Parser *parser) {
    if (!parser_expect(parser, TOKEN_CREATE, "Expected 'CREATE'")) return NULL;
    if (!parser_expect(parser, TOKEN_TABLE, "Expected 'TABLE'"))   return NULL;

    char *table_name = parse_identifier(parser);
    if (!table_name) return NULL;

    if (!parser_expect(parser, TOKEN_LPAREN, "Expected '(' after table name")) {
        free(table_name);
        return NULL;
    }

    CreateTableStmt *stmt = (CreateTableStmt *)malloc(sizeof(CreateTableStmt));
    stmt->table_name = table_name;
    stmt->columns = NULL;
    stmt->column_count = 0;

    ColumnDef **tail = &stmt->columns;

    while (parser->current.type != TOKEN_RPAREN && parser->current.type != TOKEN_EOF) {
        ColumnDef *col = parse_column_def(parser);
        if (!col) {
            // Free allocated structures on error
            // (omitted for brevity, in real DB run free_create_table_stmt)
            return NULL;
        }

        *tail = col;
        tail = &col->next;
        stmt->column_count++;

        if (!parser_match(parser, TOKEN_COMMA)) {
            break;
        }
    }

    if (!parser_expect(parser, TOKEN_RPAREN, "Expected ')' after column list")) {
        return NULL;
    }

    // Semicolon is optional
    parser_match(parser, TOKEN_SEMICOLON);

    return stmt;
}

/* ========================================================================= *
 * 4. CLEANUP & DEBUG PRINTING
 * ========================================================================= */

void free_create_table_stmt(CreateTableStmt *stmt) {
    if (!stmt) return;
    free(stmt->table_name);
    ColumnDef *curr = stmt->columns;
    while (curr) {
        ColumnDef *next = curr->next;
        free(curr->name);
        free(curr);
        curr = next;
    }
    free(stmt);
}

void print_ast(const CreateTableStmt *stmt) {
    if (!stmt) return;
    printf("CreateTableStmt:\n");
    printf("  Table Name: %s\n", stmt->table_name);
    printf("  Columns (%d):\n", stmt->column_count);

    ColumnDef *curr = stmt->columns;
    int idx = 1;
    while (curr) {
        printf("    [%d] Name: %-8s | Type: %-7s | PrimaryKey: %s\n",
               idx++,
               curr->name,
               curr->type == DATA_TYPE_INT ? "INT" : "VARCHAR",
               curr->is_primary_key ? "YES" : "NO");
        curr = curr->next;
    }
}

/* ========================================================================= *
 * 5. MAIN / DEMONSTRATION
 * ========================================================================= */

int main(void) {
    const char *query = 
        "CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, "
        "dep VARCHAR, salary INT, city VARCHAR);";

    printf("Input Query:\n%s\n\n", query);

    Lexer lexer;
    lexer_init(&lexer, query);

    Parser parser;
    parser_init(&parser, &lexer);

    CreateTableStmt *stmt = parse_create_table(&parser);

    if (stmt) {
        printf("AST successfully constructed:\n");
        print_ast(stmt);
        free_create_table_stmt(stmt);
    } else {
        printf("Failed to parse query.\n");
    }

    return 0;
}
