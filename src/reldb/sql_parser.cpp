#include "reldb/sql_parser.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

#include "reldb/macros.h"

namespace reldb {
namespace {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

enum class TokenKind : std::uint8_t {
    kEof = 0,
    kIdent,     // unquoted identifier (or unknown keyword-as-ident fallback)
    kInteger,   // decimal integer literal
    kString,    // single-quoted string
    // Keywords
    kBegin,
    kCommit,
    kAbort,
    kRollback,
    kTransaction,
    kCreate,
    kDrop,
    kAlter,
    kAdd,
    kColumn,
    kDefault,
    kTable,
    kIf,
    kExists,
    kInsert,
    kInto,
    kValues,
    kSelect,
    kFrom,
    kWhere,
    kOrder,
    kBy,
    kLimit,
    kAsc,
    kDesc,
    kUpdate,
    kSet,
    kDelete,
    kPrimary,
    kKey,
    kAnd,
    kOr,
    kNot,
    kTrue,
    kFalse,
    kNull,
    // Type names (also valid as keywords in col defs)
    kTypeInt,       // INT
    kTypeInteger,   // INTEGER
    kTypeText,      // TEXT
    kTypeString,    // STRING
    kTypeBool,      // BOOL
    kTypeBoolean,   // BOOLEAN
    // Unsupported keywords — recognized so we can give clear errors
    kJoin,
    kGroup,
    kHaving,
    kUnion,
    kIntersect,
    kExcept,
    kDistinct,
    kAs,
    // Aggregate functions
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
    // Punctuation / operators
    kLParen,
    kRParen,
    kComma,
    kSemicolon,
    kStar,
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
};

struct Token {
    TokenKind kind = TokenKind::kEof;
    std::string text;          // ident text, string contents, raw integer digits
    std::int64_t int_val = 0;  // for kInteger
    std::size_t pos = 0;       // byte offset in input (for errors)
};

const char* TokenKindName(TokenKind k) {
    switch (k) {
        case TokenKind::kEof: return "end of input";
        case TokenKind::kIdent: return "identifier";
        case TokenKind::kInteger: return "integer";
        case TokenKind::kString: return "string";
        case TokenKind::kBegin: return "BEGIN";
        case TokenKind::kCommit: return "COMMIT";
        case TokenKind::kAbort: return "ABORT";
        case TokenKind::kRollback: return "ROLLBACK";
        case TokenKind::kTransaction: return "TRANSACTION";
        case TokenKind::kCreate: return "CREATE";
        case TokenKind::kDrop: return "DROP";
        case TokenKind::kAlter: return "ALTER";
        case TokenKind::kAdd: return "ADD";
        case TokenKind::kColumn: return "COLUMN";
        case TokenKind::kDefault: return "DEFAULT";
        case TokenKind::kTable: return "TABLE";
        case TokenKind::kIf: return "IF";
        case TokenKind::kExists: return "EXISTS";
        case TokenKind::kInsert: return "INSERT";
        case TokenKind::kInto: return "INTO";
        case TokenKind::kValues: return "VALUES";
        case TokenKind::kSelect: return "SELECT";
        case TokenKind::kFrom: return "FROM";
        case TokenKind::kWhere: return "WHERE";
        case TokenKind::kOrder: return "ORDER";
        case TokenKind::kBy: return "BY";
        case TokenKind::kLimit: return "LIMIT";
        case TokenKind::kAsc: return "ASC";
        case TokenKind::kDesc: return "DESC";
        case TokenKind::kUpdate: return "UPDATE";
        case TokenKind::kSet: return "SET";
        case TokenKind::kDelete: return "DELETE";
        case TokenKind::kPrimary: return "PRIMARY";
        case TokenKind::kKey: return "KEY";
        case TokenKind::kAnd: return "AND";
        case TokenKind::kOr: return "OR";
        case TokenKind::kNot: return "NOT";
        case TokenKind::kTrue: return "TRUE";
        case TokenKind::kFalse: return "FALSE";
        case TokenKind::kNull: return "NULL";
        case TokenKind::kTypeInt: return "INT";
        case TokenKind::kTypeInteger: return "INTEGER";
        case TokenKind::kTypeText: return "TEXT";
        case TokenKind::kTypeString: return "STRING";
        case TokenKind::kTypeBool: return "BOOL";
        case TokenKind::kTypeBoolean: return "BOOLEAN";
        case TokenKind::kJoin: return "JOIN";
        case TokenKind::kGroup: return "GROUP";
        case TokenKind::kHaving: return "HAVING";
        case TokenKind::kUnion: return "UNION";
        case TokenKind::kIntersect: return "INTERSECT";
        case TokenKind::kExcept: return "EXCEPT";
        case TokenKind::kDistinct: return "DISTINCT";
        case TokenKind::kAs: return "AS";
        case TokenKind::kCount: return "COUNT";
        case TokenKind::kSum: return "SUM";
        case TokenKind::kAvg: return "AVG";
        case TokenKind::kMin: return "MIN";
        case TokenKind::kMax: return "MAX";
        case TokenKind::kLParen: return "(";
        case TokenKind::kRParen: return ")";
        case TokenKind::kComma: return ",";
        case TokenKind::kSemicolon: return ";";
        case TokenKind::kStar: return "*";
        case TokenKind::kEq: return "=";
        case TokenKind::kNe: return "!=";
        case TokenKind::kLt: return "<";
        case TokenKind::kLe: return "<=";
        case TokenKind::kGt: return ">";
        case TokenKind::kGe: return ">=";
    }
    return "token";
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input), pos_(0) { Advance(); }

    const Token& current() const { return cur_; }

    void Advance() { cur_ = NextToken(); }

    bool Check(TokenKind k) const { return cur_.kind == k; }

    bool Match(TokenKind k) {
        if (cur_.kind != k) return false;
        Advance();
        return true;
    }

    lsmkv::Status Expect(TokenKind k, const char* what) {
        if (cur_.kind != k) {
            return Error(std::string("expected ") + what + ", got " + TokenKindName(cur_.kind));
        }
        Advance();
        return STATUS(OK);
    }

    lsmkv::Status Error(const std::string& msg) const {
        return STATUS(InvalidArgument, "SQL parse error at offset " + std::to_string(cur_.pos) +
                                           ": " + msg);
    }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    Token NextToken() {
        SkipWhitespace();
        Token t;
        t.pos = pos_;
        if (pos_ >= input_.size()) {
            t.kind = TokenKind::kEof;
            return t;
        }
        const char c = input_[pos_];

        // Single-char and two-char operators / punctuation.
        switch (c) {
            case '(':
                ++pos_;
                t.kind = TokenKind::kLParen;
                return t;
            case ')':
                ++pos_;
                t.kind = TokenKind::kRParen;
                return t;
            case ',':
                ++pos_;
                t.kind = TokenKind::kComma;
                return t;
            case ';':
                ++pos_;
                t.kind = TokenKind::kSemicolon;
                return t;
            case '*':
                ++pos_;
                t.kind = TokenKind::kStar;
                return t;
            case '=':
                ++pos_;
                t.kind = TokenKind::kEq;
                return t;
            case '!':
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    pos_ += 2;
                    t.kind = TokenKind::kNe;
                    return t;
                }
                ++pos_;
                t.kind = TokenKind::kEof;
                t.text = "unexpected '!'";
                return t;
            case '<':
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    pos_ += 2;
                    t.kind = TokenKind::kLe;
                    return t;
                }
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>') {
                    pos_ += 2;
                    t.kind = TokenKind::kNe;
                    return t;
                }
                ++pos_;
                t.kind = TokenKind::kLt;
                return t;
            case '>':
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
                    pos_ += 2;
                    t.kind = TokenKind::kGe;
                    return t;
                }
                ++pos_;
                t.kind = TokenKind::kGt;
                return t;
            case '\'':
                return LexString();
            default:
                break;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && pos_ + 1 < input_.size() &&
             std::isdigit(static_cast<unsigned char>(input_[pos_ + 1])))) {
            return LexInteger();
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            return LexIdentOrKeyword();
        }

        ++pos_;
        t.kind = TokenKind::kEof;
        t.text = std::string("unexpected character '") + c + "'";
        return t;
    }

    Token LexString() {
        Token t;
        t.pos = pos_;
        ++pos_;  // opening '
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == '\'') {
                if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '\'') {
                    out.push_back('\'');
                    pos_ += 2;
                    continue;
                }
                ++pos_;  // closing '
                t.kind = TokenKind::kString;
                t.text = std::move(out);
                return t;
            }
            out.push_back(c);
            ++pos_;
        }
        t.kind = TokenKind::kEof;
        t.text = "unterminated string literal";
        return t;
    }

    Token LexInteger() {
        Token t;
        t.pos = pos_;
        std::size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
        t.text = std::string(input_.substr(start, pos_ - start));
        // Parse int64; overflow → still store text and fail later.
        try {
            std::size_t idx = 0;
            t.int_val = std::stoll(t.text, &idx, 10);
            if (idx != t.text.size()) {
                t.kind = TokenKind::kIdent;
                return t;
            }
        } catch (...) {
            t.kind = TokenKind::kIdent;
            t.text = "integer out of range";
            return t;
        }
        t.kind = TokenKind::kInteger;
        return t;
    }

    Token LexIdentOrKeyword() {
        Token t;
        t.pos = pos_;
        std::size_t start = pos_;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_]);
            if (std::isalnum(c) || c == '_') {
                ++pos_;
            } else {
                break;
            }
        }
        t.text = std::string(input_.substr(start, pos_ - start));
        // Lowercase copy for keyword match; preserve original case in t.text for idents.
        std::string lower;
        lower.reserve(t.text.size());
        for (char ch : t.text) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        t.kind = KeywordKind(lower);
        return t;
    }

    static TokenKind KeywordKind(const std::string& lower) {
        if (lower == "begin") return TokenKind::kBegin;
        if (lower == "commit") return TokenKind::kCommit;
        if (lower == "abort") return TokenKind::kAbort;
        if (lower == "rollback") return TokenKind::kRollback;
        if (lower == "transaction") return TokenKind::kTransaction;
        if (lower == "create") return TokenKind::kCreate;
        if (lower == "drop") return TokenKind::kDrop;
        if (lower == "alter") return TokenKind::kAlter;
        if (lower == "add") return TokenKind::kAdd;
        if (lower == "column") return TokenKind::kColumn;
        if (lower == "default") return TokenKind::kDefault;
        if (lower == "table") return TokenKind::kTable;
        if (lower == "if") return TokenKind::kIf;
        if (lower == "exists") return TokenKind::kExists;
        if (lower == "insert") return TokenKind::kInsert;
        if (lower == "into") return TokenKind::kInto;
        if (lower == "values") return TokenKind::kValues;
        if (lower == "select") return TokenKind::kSelect;
        if (lower == "from") return TokenKind::kFrom;
        if (lower == "where") return TokenKind::kWhere;
        if (lower == "order") return TokenKind::kOrder;
        if (lower == "by") return TokenKind::kBy;
        if (lower == "limit") return TokenKind::kLimit;
        if (lower == "asc") return TokenKind::kAsc;
        if (lower == "desc") return TokenKind::kDesc;
        if (lower == "update") return TokenKind::kUpdate;
        if (lower == "set") return TokenKind::kSet;
        if (lower == "delete") return TokenKind::kDelete;
        if (lower == "primary") return TokenKind::kPrimary;
        if (lower == "key") return TokenKind::kKey;
        if (lower == "and") return TokenKind::kAnd;
        if (lower == "or") return TokenKind::kOr;
        if (lower == "not") return TokenKind::kNot;
        if (lower == "true") return TokenKind::kTrue;
        if (lower == "false") return TokenKind::kFalse;
        if (lower == "null") return TokenKind::kNull;
        if (lower == "int") return TokenKind::kTypeInt;
        if (lower == "integer") return TokenKind::kTypeInteger;
        if (lower == "text") return TokenKind::kTypeText;
        if (lower == "string") return TokenKind::kTypeString;
        if (lower == "bool") return TokenKind::kTypeBool;
        if (lower == "boolean") return TokenKind::kTypeBoolean;
        if (lower == "join") return TokenKind::kJoin;
        if (lower == "inner" || lower == "left" || lower == "right" || lower == "full" ||
            lower == "outer" || lower == "cross") {
            // Treat join-ish keywords as unsupported JOIN family at parse sites.
            return TokenKind::kJoin;
        }
        if (lower == "group") return TokenKind::kGroup;
        if (lower == "having") return TokenKind::kHaving;
        if (lower == "union") return TokenKind::kUnion;
        if (lower == "intersect") return TokenKind::kIntersect;
        if (lower == "except") return TokenKind::kExcept;
        if (lower == "distinct") return TokenKind::kDistinct;
        if (lower == "as") return TokenKind::kAs;
        if (lower == "count") return TokenKind::kCount;
        if (lower == "sum") return TokenKind::kSum;
        if (lower == "avg") return TokenKind::kAvg;
        if (lower == "min") return TokenKind::kMin;
        if (lower == "max") return TokenKind::kMax;
        return TokenKind::kIdent;
    }

    std::string_view input_;
    std::size_t pos_;
    Token cur_;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view sql) : lex_(sql) {}

    lsmkv::Status ParseScript(std::vector<Statement>* out) {
        if (out == nullptr) return STATUS(InvalidArgument, "null out");
        out->clear();
        // Allow empty script.
        while (!lex_.Check(TokenKind::kEof)) {
            // Skip empty statements (extra semicolons).
            if (lex_.Match(TokenKind::kSemicolon)) continue;
            Statement stmt;
            RELDB_RETURN_NOT_OK(ParseOne(&stmt));
            out->push_back(std::move(stmt));
            if (lex_.Check(TokenKind::kEof)) break;
            if (lex_.Match(TokenKind::kSemicolon)) {
                continue;
            }
            // Next statement must be preceded by ';' if more tokens remain.
            // If we already consumed trailing semicolon inside ParseOne? we don't.
            return lex_.Error(std::string("expected ';' between statements, got ") +
                              TokenKindName(lex_.current().kind));
        }
        return STATUS(OK);
    }

    lsmkv::Status ParseOneStatement(Statement* out) {
        if (out == nullptr) return STATUS(InvalidArgument, "null out");
        // Skip leading empty ;
        while (lex_.Match(TokenKind::kSemicolon)) {
        }
        if (lex_.Check(TokenKind::kEof)) {
            return lex_.Error("empty statement");
        }
        RELDB_RETURN_NOT_OK(ParseOne(out));
        lex_.Match(TokenKind::kSemicolon);  // optional trailing
        if (!lex_.Check(TokenKind::kEof)) {
            return lex_.Error(std::string("unexpected trailing token ") +
                              TokenKindName(lex_.current().kind));
        }
        return STATUS(OK);
    }

private:
    lsmkv::Status ParseOne(Statement* out) {
        // Unsupported top-level keywords first for clear messages.
        RELDB_RETURN_NOT_OK(RejectUnsupportedStatementStart());

        switch (lex_.current().kind) {
            case TokenKind::kBegin:
                return ParseBegin(out);
            case TokenKind::kCommit:
                return ParseCommit(out);
            case TokenKind::kAbort:
            case TokenKind::kRollback:
                return ParseAbort(out);
            case TokenKind::kCreate:
                return ParseCreateTable(out);
            case TokenKind::kDrop:
                return ParseDropTable(out);
            case TokenKind::kAlter:
                return ParseAlterTable(out);
            case TokenKind::kInsert:
                return ParseInsert(out);
            case TokenKind::kSelect:
                return ParseSelect(out);
            case TokenKind::kUpdate:
                return ParseUpdate(out);
            case TokenKind::kDelete:
                return ParseDelete(out);
            case TokenKind::kEof:
                return lex_.Error("empty statement");
            default:
                return lex_.Error(std::string("unexpected token ") +
                                  TokenKindName(lex_.current().kind) + " at start of statement");
        }
    }

    lsmkv::Status RejectUnsupportedStatementStart() {
        switch (lex_.current().kind) {
            case TokenKind::kJoin:
                return lex_.Error("JOIN is not supported");
            case TokenKind::kGroup:
                return lex_.Error("GROUP BY is not supported");
            case TokenKind::kHaving:
                return lex_.Error("HAVING is not supported");
            case TokenKind::kUnion:
                return lex_.Error("UNION is not supported");
            case TokenKind::kIntersect:
                return lex_.Error("INTERSECT is not supported");
            case TokenKind::kExcept:
                return lex_.Error("EXCEPT is not supported");
            case TokenKind::kDistinct:
                return lex_.Error("DISTINCT is not supported");
            default:
                return STATUS(OK);
        }
    }

    lsmkv::Status ParseBegin(Statement* out) {
        lex_.Advance();  // BEGIN
        lex_.Match(TokenKind::kTransaction);  // optional
        *out = BeginStmt{};
        return STATUS(OK);
    }

    lsmkv::Status ParseCommit(Statement* out) {
        lex_.Advance();
        lex_.Match(TokenKind::kTransaction);
        *out = CommitStmt{};
        return STATUS(OK);
    }

    lsmkv::Status ParseAbort(Statement* out) {
        lex_.Advance();  // ABORT | ROLLBACK
        lex_.Match(TokenKind::kTransaction);
        *out = AbortStmt{};
        return STATUS(OK);
    }

    lsmkv::Status ParseCreateTable(Statement* out) {
        lex_.Advance();  // CREATE
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kTable, "TABLE"));
        std::string name;
        RELDB_RETURN_NOT_OK(ParseIdent(&name));
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kLParen, "'('"));

        CreateTableStmt stmt;
        stmt.table_name = std::move(name);
        int pk_count = 0;
        for (;;) {
            ColumnDefAst col;
            RELDB_RETURN_NOT_OK(ParseIdent(&col.name));
            RELDB_RETURN_NOT_OK(ParseColumnType(&col.type));
            if (lex_.Match(TokenKind::kPrimary)) {
                RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kKey, "KEY"));
                col.primary_key = true;
                ++pk_count;
            }
            stmt.columns.push_back(std::move(col));
            if (lex_.Match(TokenKind::kComma)) continue;
            break;
        }
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
        if (pk_count != 1) {
            return lex_.Error("CREATE TABLE requires exactly one PRIMARY KEY column");
        }
        *out = std::move(stmt);
        return STATUS(OK);
    }

    lsmkv::Status ParseDropTable(Statement* out) {
        lex_.Advance();  // DROP
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kTable, "TABLE"));
        DropTableStmt stmt;
        // Optional: IF EXISTS
        if (lex_.Match(TokenKind::kIf)) {
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kExists, "EXISTS"));
            stmt.if_exists = true;
        }
        std::string name;
        RELDB_RETURN_NOT_OK(ParseIdent(&name));
        stmt.table_name = std::move(name);
        *out = std::move(stmt);
        return STATUS(OK);
    }

    // ALTER TABLE name ( ADD COLUMN name type DEFAULT literal
    //                  | DROP COLUMN name )
    lsmkv::Status ParseAlterTable(Statement* out) {
        lex_.Advance();  // ALTER
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kTable, "TABLE"));
        std::string table;
        RELDB_RETURN_NOT_OK(ParseIdent(&table));

        if (lex_.Match(TokenKind::kAdd)) {
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kColumn, "COLUMN"));
            AlterTableAddColumnStmt stmt;
            stmt.table_name = std::move(table);
            RELDB_RETURN_NOT_OK(ParseIdent(&stmt.column.name));
            RELDB_RETURN_NOT_OK(ParseColumnType(&stmt.column.type));
            // ALTER ADD does not allow PRIMARY KEY (grammar: alter_col_def := name type).
            if (lex_.Match(TokenKind::kPrimary)) {
                return lex_.Error("ALTER ADD COLUMN cannot add a PRIMARY KEY");
            }
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kDefault, "DEFAULT"));
            RELDB_RETURN_NOT_OK(ParseLiteralValue(&stmt.default_value));
            stmt.column.primary_key = false;
            *out = std::move(stmt);
            return STATUS(OK);
        }

        if (lex_.Match(TokenKind::kDrop)) {
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kColumn, "COLUMN"));
            AlterTableDropColumnStmt stmt;
            stmt.table_name = std::move(table);
            RELDB_RETURN_NOT_OK(ParseIdent(&stmt.column_name));
            *out = std::move(stmt);
            return STATUS(OK);
        }

        return lex_.Error(std::string("expected ADD or DROP after ALTER TABLE, got ") +
                          TokenKindName(lex_.current().kind));
    }

    lsmkv::Status ParseColumnType(ColumnType* out) {
        switch (lex_.current().kind) {
            case TokenKind::kTypeInt:
            case TokenKind::kTypeInteger:
                *out = ColumnType::kInt64;
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kTypeText:
            case TokenKind::kTypeString:
                *out = ColumnType::kString;
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kTypeBool:
            case TokenKind::kTypeBoolean:
                *out = ColumnType::kBool;
                lex_.Advance();
                return STATUS(OK);
            default:
                return lex_.Error("expected column type (INT, TEXT, BOOL, …)");
        }
    }

    lsmkv::Status ParseInsert(Statement* out) {
        lex_.Advance();  // INSERT
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kInto, "INTO"));
        InsertStmt stmt;
        RELDB_RETURN_NOT_OK(ParseIdent(&stmt.table_name));
        if (lex_.Match(TokenKind::kLParen)) {
            for (;;) {
                std::string col;
                RELDB_RETURN_NOT_OK(ParseIdent(&col));
                stmt.column_names.push_back(std::move(col));
                if (lex_.Match(TokenKind::kComma)) continue;
                break;
            }
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
        }
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kValues, "VALUES"));
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kLParen, "'('"));
        for (;;) {
            Value v;
            RELDB_RETURN_NOT_OK(ParseLiteralValue(&v));
            stmt.values.push_back(std::move(v));
            if (lex_.Match(TokenKind::kComma)) continue;
            break;
        }
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
        // Reject multi-row: VALUES (...), (...)
        if (lex_.Check(TokenKind::kComma)) {
            return lex_.Error("multi-row INSERT is not supported");
        }
        if (!stmt.column_names.empty() && stmt.column_names.size() != stmt.values.size()) {
            return lex_.Error("INSERT column count does not match VALUES count");
        }
        *out = std::move(stmt);
        return STATUS(OK);
    }

    static bool IsAggFunc(TokenKind k) {
        return k == TokenKind::kCount || k == TokenKind::kSum || k == TokenKind::kAvg ||
               k == TokenKind::kMin || k == TokenKind::kMax;
    }

    static AggFunc TokenToAggFunc(TokenKind k) {
        switch (k) {
            case TokenKind::kCount: return AggFunc::kCount;
            case TokenKind::kSum: return AggFunc::kSum;
            case TokenKind::kAvg: return AggFunc::kAvg;
            case TokenKind::kMin: return AggFunc::kMin;
            case TokenKind::kMax: return AggFunc::kMax;
            default: return AggFunc::kCount;
        }
    }

    // select_item := agg_func '(' ( '*' | column_ref ) ')' [ AS name ]
    //              | expr [ AS name ]
    lsmkv::Status ParseSelectItem(SelectItem* out) {
        if (IsAggFunc(lex_.current().kind)) {
            const TokenKind func_tok = lex_.current().kind;
            lex_.Advance();
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kLParen, "'('"));

            SelectItem item;
            item.kind = SelectItem::Kind::kAgg;
            item.agg_func = TokenToAggFunc(func_tok);

            if (lex_.Match(TokenKind::kStar)) {
                if (func_tok != TokenKind::kCount) {
                    return lex_.Error("only COUNT(*) is allowed; use a column for other aggregates");
                }
                item.agg_star = true;
            } else {
                // Single column ref only (no SUM(a+b)).
                RELDB_RETURN_NOT_OK(ParseIdent(&item.agg_column));
                item.agg_star = false;
            }
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
            if (lex_.Match(TokenKind::kAs)) {
                RELDB_RETURN_NOT_OK(ParseIdent(&item.alias));
            }
            *out = std::move(item);
            return STATUS(OK);
        }

        std::unique_ptr<Expr> e;
        RELDB_RETURN_NOT_OK(ParseExpr(&e));
        SelectItem item = MakeExprSelectItem(std::move(e));
        if (lex_.Match(TokenKind::kAs)) {
            RELDB_RETURN_NOT_OK(ParseIdent(&item.alias));
        }
        *out = std::move(item);
        return STATUS(OK);
    }

    lsmkv::Status ParseSelect(Statement* out) {
        lex_.Advance();  // SELECT
        if (lex_.Check(TokenKind::kDistinct)) {
            return lex_.Error("SELECT DISTINCT is not supported");
        }
        SelectStmt stmt;
        if (lex_.Match(TokenKind::kStar)) {
            stmt.select_star = true;
        } else {
            for (;;) {
                if (lex_.Check(TokenKind::kStar)) {
                    return lex_.Error("cannot mix * with other select items");
                }
                SelectItem item;
                RELDB_RETURN_NOT_OK(ParseSelectItem(&item));
                stmt.select_list.push_back(std::move(item));
                if (lex_.Match(TokenKind::kComma)) continue;
                break;
            }
            if (stmt.select_list.empty()) {
                return lex_.Error("empty SELECT list");
            }
        }
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kFrom, "FROM"));
        if (lex_.Check(TokenKind::kLParen)) {
            return lex_.Error("subqueries are not supported");
        }
        RELDB_RETURN_NOT_OK(ParseIdent(&stmt.from.table_name));
        // Reject JOIN after table.
        if (lex_.Check(TokenKind::kJoin) || lex_.Check(TokenKind::kComma)) {
            return lex_.Error("joins are not supported");
        }

        if (lex_.Match(TokenKind::kWhere)) {
            RELDB_RETURN_NOT_OK(ParseExpr(&stmt.where));
        }
        if (lex_.Match(TokenKind::kGroup)) {
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kBy, "BY"));
            for (;;) {
                std::string col;
                RELDB_RETURN_NOT_OK(ParseIdent(&col));
                stmt.group_by.push_back(std::move(col));
                if (lex_.Match(TokenKind::kComma)) continue;
                break;
            }
            if (stmt.group_by.empty()) {
                return lex_.Error("empty GROUP BY list");
            }
        }
        if (lex_.Match(TokenKind::kHaving)) {
            // HAVING expressions may reference group columns, aggregate output
            // names, and agg call syntax rewritten to those names (COUNT(*) etc.).
            having_context_ = true;
            auto st = ParseExpr(&stmt.having);
            having_context_ = false;
            RELDB_RETURN_NOT_OK(st);
        }
        if (lex_.Match(TokenKind::kOrder)) {
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kBy, "BY"));
            for (;;) {
                OrderByItem item;
                RELDB_RETURN_NOT_OK(ParseIdent(&item.column_name));
                if (lex_.Match(TokenKind::kDesc)) {
                    item.ascending = false;
                } else {
                    lex_.Match(TokenKind::kAsc);  // optional ASC
                    item.ascending = true;
                }
                stmt.order_by.push_back(std::move(item));
                if (lex_.Match(TokenKind::kComma)) continue;
                break;
            }
        }
        if (lex_.Match(TokenKind::kLimit)) {
            if (!lex_.Check(TokenKind::kInteger)) {
                return lex_.Error("LIMIT requires an integer");
            }
            if (lex_.current().int_val < 0) {
                return lex_.Error("LIMIT must be non-negative");
            }
            stmt.has_limit = true;
            stmt.limit = lex_.current().int_val;
            lex_.Advance();
        }
        // Trailing unsupported set ops
        if (lex_.Check(TokenKind::kUnion) || lex_.Check(TokenKind::kIntersect) ||
            lex_.Check(TokenKind::kExcept)) {
            return lex_.Error("set operations are not supported");
        }
        *out = std::move(stmt);
        return STATUS(OK);
    }

    lsmkv::Status ParseUpdate(Statement* out) {
        lex_.Advance();  // UPDATE
        UpdateStmt stmt;
        RELDB_RETURN_NOT_OK(ParseIdent(&stmt.table_name));
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kSet, "SET"));
        for (;;) {
            AssignmentAst a;
            RELDB_RETURN_NOT_OK(ParseIdent(&a.column_name));
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kEq, "'='"));
            RELDB_RETURN_NOT_OK(ParseExpr(&a.value));
            stmt.sets.push_back(std::move(a));
            if (lex_.Match(TokenKind::kComma)) continue;
            break;
        }
        if (stmt.sets.empty()) {
            return lex_.Error("UPDATE requires at least one assignment");
        }
        if (lex_.Match(TokenKind::kWhere)) {
            RELDB_RETURN_NOT_OK(ParseExpr(&stmt.where));
        }
        *out = std::move(stmt);
        return STATUS(OK);
    }

    lsmkv::Status ParseDelete(Statement* out) {
        lex_.Advance();  // DELETE
        RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kFrom, "FROM"));
        DeleteStmt stmt;
        RELDB_RETURN_NOT_OK(ParseIdent(&stmt.table_name));
        if (lex_.Match(TokenKind::kWhere)) {
            RELDB_RETURN_NOT_OK(ParseExpr(&stmt.where));
        }
        *out = std::move(stmt);
        return STATUS(OK);
    }

    // Identifiers: kIdent or type-name keywords used as names (edge case).
    lsmkv::Status ParseIdent(std::string* out) {
        if (lex_.Check(TokenKind::kIdent)) {
            *out = lex_.current().text;
            lex_.Advance();
            return STATUS(OK);
        }
        // Only bare identifiers (kIdent); reserved keywords are not allowed as names.
        return lex_.Error(std::string("expected identifier, got ") +
                          TokenKindName(lex_.current().kind));
    }

    lsmkv::Status ParseLiteralValue(Value* out) {
        switch (lex_.current().kind) {
            case TokenKind::kInteger:
                *out = Value::Int64(lex_.current().int_val);
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kString:
                // Detect unterminated: LexString sets kEof with message.
                *out = Value::String(lex_.current().text);
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kTrue:
                *out = Value::Bool(true);
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kFalse:
                *out = Value::Bool(false);
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kNull:
                *out = Value::Null();
                lex_.Advance();
                return STATUS(OK);
            case TokenKind::kEof:
                if (!lex_.current().text.empty()) {
                    return lex_.Error(lex_.current().text);
                }
                return lex_.Error("expected literal");
            default:
                return lex_.Error(std::string("expected literal, got ") +
                                  TokenKindName(lex_.current().kind));
        }
    }

    // expr := or_expr
    lsmkv::Status ParseExpr(std::unique_ptr<Expr>* out) { return ParseOr(out); }

    lsmkv::Status ParseOr(std::unique_ptr<Expr>* out) {
        std::unique_ptr<Expr> left;
        RELDB_RETURN_NOT_OK(ParseAnd(&left));
        while (lex_.Match(TokenKind::kOr)) {
            std::unique_ptr<Expr> right;
            RELDB_RETURN_NOT_OK(ParseAnd(&right));
            left = Expr::Or(std::move(left), std::move(right));
        }
        *out = std::move(left);
        return STATUS(OK);
    }

    lsmkv::Status ParseAnd(std::unique_ptr<Expr>* out) {
        std::unique_ptr<Expr> left;
        RELDB_RETURN_NOT_OK(ParseNot(&left));
        while (lex_.Match(TokenKind::kAnd)) {
            std::unique_ptr<Expr> right;
            RELDB_RETURN_NOT_OK(ParseNot(&right));
            left = Expr::And(std::move(left), std::move(right));
        }
        *out = std::move(left);
        return STATUS(OK);
    }

    lsmkv::Status ParseNot(std::unique_ptr<Expr>* out) {
        if (lex_.Match(TokenKind::kNot)) {
            std::unique_ptr<Expr> child;
            RELDB_RETURN_NOT_OK(ParseNot(&child));
            *out = Expr::Not(std::move(child));
            return STATUS(OK);
        }
        return ParseCmp(out);
    }

    lsmkv::Status ParseCmp(std::unique_ptr<Expr>* out) {
        std::unique_ptr<Expr> left;
        RELDB_RETURN_NOT_OK(ParsePrimary(&left));
        CmpOp op;
        if (!MatchCmpOp(&op)) {
            *out = std::move(left);
            return STATUS(OK);
        }
        std::unique_ptr<Expr> right;
        RELDB_RETURN_NOT_OK(ParsePrimary(&right));
        *out = Expr::Compare(op, std::move(left), std::move(right));
        return STATUS(OK);
    }

    bool MatchCmpOp(CmpOp* op) {
        switch (lex_.current().kind) {
            case TokenKind::kEq:
                *op = CmpOp::kEq;
                lex_.Advance();
                return true;
            case TokenKind::kNe:
                *op = CmpOp::kNe;
                lex_.Advance();
                return true;
            case TokenKind::kLt:
                *op = CmpOp::kLt;
                lex_.Advance();
                return true;
            case TokenKind::kLe:
                *op = CmpOp::kLe;
                lex_.Advance();
                return true;
            case TokenKind::kGt:
                *op = CmpOp::kGt;
                lex_.Advance();
                return true;
            case TokenKind::kGe:
                *op = CmpOp::kGe;
                lex_.Advance();
                return true;
            default:
                return false;
        }
    }

    // Default aggregate result name (no alias): COUNT(*) / SUM(col) / …
    static std::string DefaultAggResultName(AggFunc f, bool star, const std::string& col) {
        std::string s;
        switch (f) {
            case AggFunc::kCount: s = "COUNT"; break;
            case AggFunc::kSum: s = "SUM"; break;
            case AggFunc::kAvg: s = "AVG"; break;
            case AggFunc::kMin: s = "MIN"; break;
            case AggFunc::kMax: s = "MAX"; break;
        }
        s += '(';
        if (star) {
            s += '*';
        } else {
            s += col;
        }
        s += ')';
        return s;
    }

    lsmkv::Status ParsePrimary(std::unique_ptr<Expr>* out) {
        if (lex_.Match(TokenKind::kLParen)) {
            RELDB_RETURN_NOT_OK(ParseExpr(out));
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
            return STATUS(OK);
        }
        // In HAVING, agg_func(...) is a reference to the aggregate result column.
        if (having_context_ && IsAggFunc(lex_.current().kind)) {
            const TokenKind func_tok = lex_.current().kind;
            lex_.Advance();
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kLParen, "'('"));
            const AggFunc func = TokenToAggFunc(func_tok);
            bool star = false;
            std::string col;
            if (lex_.Match(TokenKind::kStar)) {
                if (func_tok != TokenKind::kCount) {
                    return lex_.Error("only COUNT(*) is allowed; use a column for other aggregates");
                }
                star = true;
            } else {
                RELDB_RETURN_NOT_OK(ParseIdent(&col));
            }
            RELDB_RETURN_NOT_OK(lex_.Expect(TokenKind::kRParen, "')'"));
            *out = Expr::Column(DefaultAggResultName(func, star, col));
            return STATUS(OK);
        }
        // Literals
        switch (lex_.current().kind) {
            case TokenKind::kInteger:
            case TokenKind::kString:
            case TokenKind::kTrue:
            case TokenKind::kFalse:
            case TokenKind::kNull: {
                Value v;
                RELDB_RETURN_NOT_OK(ParseLiteralValue(&v));
                *out = Expr::Literal(std::move(v));
                return STATUS(OK);
            }
            case TokenKind::kIdent: {
                std::string name = lex_.current().text;
                lex_.Advance();
                *out = Expr::Column(std::move(name));
                return STATUS(OK);
            }
            case TokenKind::kEof:
                if (!lex_.current().text.empty()) {
                    return lex_.Error(lex_.current().text);
                }
                return lex_.Error("expected expression");
            default:
                return lex_.Error(std::string("expected expression, got ") +
                                  TokenKindName(lex_.current().kind));
        }
    }

    Lexer lex_;
    bool having_context_ = false;
};

}  // namespace

lsmkv::Status ParseScript(std::string_view sql, std::vector<Statement>* out) {
    Parser p(sql);
    return p.ParseScript(out);
}

lsmkv::Status ParseStatement(std::string_view sql, Statement* out) {
    Parser p(sql);
    return p.ParseOneStatement(out);
}

}  // namespace reldb
