#ifndef AST_HPP
#define AST_HPP

#include "Schema.hpp"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

enum class ExecuteResult {
    SUCCESS,
    DUPLICATE_KEY,
    NOT_FOUND,
    TX_ALREADY_ACTIVE,
    NO_ACTIVE_TX,
    TABLE_FULL
};

enum class MetaCommandResult {
    SUCCESS,
    UNRECOGNIZED_COMMAND
};

enum class PrepareResult {
    SUCCESS,
    NEGATIVE_ID,
    STRING_TOO_LONG,
    SYNTAX_ERROR,
    UNRECOGNIZED_STATEMENT
};

enum class StatementType {
    INSERT,
    SELECT,
    UPDATE,
    DELETE,
    CREATE,
    BEGIN,
    COMMIT,
    ROLLBACK
};

enum class WhereOp {
    EQ,
    NE,
    GT,
    LT,
    GE,
    LE
};

struct Expr {
    virtual ~Expr() = default;
    virtual bool evaluate(const DynamicRow& row, const Schema& schema) const = 0;
};

struct Literal {
    bool is_string{false};
    uint32_t int_value{0};
    std::string str_value;
};

struct ComparisonExpr : Expr {
    std::string column;
    WhereOp op{WhereOp::EQ};
    Literal value;

    bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        int col_idx = schema.find_column(column);
        if (col_idx < 0 || static_cast<size_t>(col_idx) >= row.values.size()) return false;

        const auto& col_def = schema.columns[col_idx];
        const auto& cell_val = row.values[col_idx];

        int cmp = 0;
        if (col_def.type == DataType::INT) {
            int32_t v = static_cast<int32_t>(value.int_value);
            cmp = (cell_val.int_val > v) - (cell_val.int_val < v);
        } else {
            int c = std::strcmp(cell_val.str_val.c_str(), value.str_value.c_str());
            cmp = (c > 0) - (c < 0);
        }

        switch (op) {
            case WhereOp::EQ: return cmp == 0;
            case WhereOp::NE: return cmp != 0;
            case WhereOp::GT: return cmp > 0;
            case WhereOp::LT: return cmp < 0;
            case WhereOp::GE: return cmp >= 0;
            case WhereOp::LE: return cmp <= 0;
        }
        return false;
    }
};

struct LogicalExpr : Expr {
    enum class LogicOp { AND, OR } op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    LogicalExpr(LogicOp op_, std::unique_ptr<Expr> left_, std::unique_ptr<Expr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        if (op == LogicOp::AND) return left->evaluate(row, schema) && right->evaluate(row, schema);
        return left->evaluate(row, schema) || right->evaluate(row, schema);
    }
};

struct UpdateAssignment {
    std::string column_name;
    std::string value_text;
};

struct Statement {
    StatementType type{StatementType::SELECT};
    DynamicRow row_to_insert;
    std::string table_name;
    Schema created_schema;

    std::unique_ptr<Expr> where;
    bool is_count{false};
    uint32_t target_id{0};

    // SET-style UPDATE
    bool is_set_update{false};
    std::vector<UpdateAssignment> update_assignments;
};

#endif // AST_HPP
