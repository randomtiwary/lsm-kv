#include <string>

#include "test_harness.h"

#include "lsmkv/status.h"
#include "protocol.h"
#include "reldb/query_result.h"
#include "reldb/row.h"
#include "reldb/types.h"

namespace {

using reldb::sqlproto::DecodeCell;
using reldb::sqlproto::DecodeReply;
using reldb::sqlproto::DecodedReply;
using reldb::sqlproto::EncodeCell;
using reldb::sqlproto::EncodeError;
using reldb::sqlproto::EncodeOk;
using reldb::sqlproto::EncodeOkRowsAffected;
using reldb::sqlproto::EncodePong;
using reldb::sqlproto::EncodeQueryResult;
using reldb::sqlproto::EncodeStatus;
using reldb::sqlproto::EndsWithStatementTerminator;
using reldb::sqlproto::ReplyKind;
using reldb::sqlproto::TryAppendLine;
using reldb::sqlproto::kMaxStatementBytes;

}  // namespace

TEST(sql_protocol_encode_decode_cells) {
    reldb::Value v;
    EXPECT_OK(DecodeCell(EncodeCell(reldb::Value::Null()), &v), "null");
    expect(v.IsNull(), "is null");

    EXPECT_OK(DecodeCell(EncodeCell(reldb::Value::Int64(-42)), &v), "int");
    expect_eq(v.GetInt64(), static_cast<std::int64_t>(-42), "int val");

    EXPECT_OK(DecodeCell(EncodeCell(reldb::Value::Bool(true)), &v), "bool t");
    expect(v.GetBool(), "bool true");
    EXPECT_OK(DecodeCell(EncodeCell(reldb::Value::Bool(false)), &v), "bool f");
    expect(!v.GetBool(), "bool false");

    EXPECT_OK(DecodeCell(EncodeCell(reldb::Value::String("ada")), &v), "str");
    expect_eq(v.GetString(), std::string("ada"), "ada");

    // Null vs string "NULL"
    expect_eq(EncodeCell(reldb::Value::Null()), std::string("$N"), "wire null");
    expect_eq(EncodeCell(reldb::Value::String("NULL")), std::string("$S:NULL"), "wire NULL str");

    // Empty string
    expect_eq(EncodeCell(reldb::Value::String("")), std::string("$S:"), "empty");
    EXPECT_OK(DecodeCell("$S:", &v), "decode empty");
    expect_eq(v.GetString(), std::string(""), "empty val");

    // Escapes: newline, tab, backslash, control byte 0x01
    const std::string raw = std::string("a\nb") + "\t" + "\\" + "\x01";
    const std::string wire = EncodeCell(reldb::Value::String(raw));
    // Wire form: $S:a\nb\t\\\x01
    expect_eq(wire, std::string("$S:a\\nb\\t\\\\\\x") + "01", "escapes wire");
    EXPECT_OK(DecodeCell(wire, &v), "decode escapes");
    expect_eq(v.GetString(), raw, "roundtrip escapes");
}

TEST(sql_protocol_bad_cell_decode) {
    reldb::Value v;
    expect(DecodeCell("$S:\\q", &v).IsInvalidArgument(), "unknown escape");
    expect(DecodeCell("$S:\\x", &v).IsInvalidArgument(), "trunc x");
    expect(DecodeCell("$S:\\x0", &v).IsInvalidArgument(), "trunc x1");
    expect(DecodeCell("$S:\\xGG", &v).IsInvalidArgument(), "bad hex");
    expect(DecodeCell("$X:1", &v).IsInvalidArgument(), "unknown form");
    expect(DecodeCell("$I:abc", &v).IsInvalidArgument(), "bad int");
    expect(DecodeCell("$B:2", &v).IsInvalidArgument(), "bad bool");
    expect(DecodeCell("", &v).IsInvalidArgument(), "empty");
}

TEST(sql_protocol_encode_error_and_ok) {
    const auto err = EncodeError(lsmkv::Status::InvalidArgument(
        "DDL is not allowed inside a transaction"));
    expect_eq(err, std::string("-ERR InvalidArgument: DDL is not allowed inside a transaction\n"),
              "err ddl");

    expect_eq(EncodeError(lsmkv::Status::Conflict("write-write conflict")),
              std::string("-ERR Conflict: write-write conflict\n"), "err conflict");

    expect_eq(EncodeError(lsmkv::Status::NotFound("table not found: missing")),
              std::string("-ERR NotFound: table not found: missing\n"), "err notfound");

    expect_eq(EncodeOk(), std::string("+OK\n"), "ok");
    expect_eq(EncodeOkRowsAffected(1), std::string("+OK rows_affected=1\n"), "ra");
    expect_eq(EncodePong(), std::string("+PONG\n"), "pong");
    expect_eq(EncodeStatus(true), std::string("+OK in_txn=1\n"), "txn1");
    expect_eq(EncodeStatus(false), std::string("+OK in_txn=0\n"), "txn0");
}

TEST(sql_protocol_encode_query_result_select) {
    reldb::QueryResult r;
    r.plan_tag = "PkPointGet";
    r.column_names = {"id", "name"};
    r.rows.push_back(reldb::Row({reldb::Value::Int64(1), reldb::Value::String("ada")}));

    const std::string wire = EncodeQueryResult(r);
    const std::string expect_wire =
        "*PLAN PkPointGet\n"
        "*RESULT 1 2\n"
        "*COLS 2\n"
        "id\n"
        "name\n"
        "*ROW\n"
        "$I:1\n"
        "$S:ada\n"
        "*END\n";
    expect_eq(wire, expect_wire, "select wire");

    DecodedReply dec;
    EXPECT_OK(DecodeReply(wire, &dec), "decode select");
    expect(dec.kind == ReplyKind::kResult, "kind result");
    expect_eq(dec.plan_tag, std::string("PkPointGet"), "plan");
    expect_eq(dec.result.rows.size(), static_cast<std::size_t>(1), "nrows");
    expect_eq(dec.result.column_names[0], std::string("id"), "col0");
    expect_eq(dec.result.rows[0].at(1).GetString(), std::string("ada"), "ada");
}

TEST(sql_protocol_encode_query_result_empty_select_and_dml) {
    reldb::QueryResult empty_sel;
    empty_sel.column_names = {"id"};
    const std::string z = EncodeQueryResult(empty_sel);
    expect_eq(z,
              std::string("*RESULT 0 1\n*COLS 1\nid\n*END\n"), "zero rows");

    reldb::QueryResult dml;
    dml.rows_affected = 3;
    expect_eq(EncodeQueryResult(dml), std::string("+OK rows_affected=3\n"), "dml");

    reldb::QueryResult ok;
    expect_eq(EncodeQueryResult(ok), std::string("+OK\n"), "plain ok");
}

TEST(sql_protocol_encode_null_cell_in_result) {
    reldb::QueryResult r;
    r.column_names = {"SUM(n)"};
    r.rows.push_back(reldb::Row({reldb::Value::Null()}));
    const std::string wire = EncodeQueryResult(r);
    expect(wire.find("$N\n") != std::string::npos, "has null cell");
    DecodedReply dec;
    EXPECT_OK(DecodeReply(wire, &dec), "decode");
    expect(dec.result.rows[0].at(0).IsNull(), "null val");
}

TEST(sql_protocol_decode_simple_replies) {
    DecodedReply d;
    EXPECT_OK(DecodeReply("+PONG\n", &d), "pong");
    expect(d.kind == ReplyKind::kPong, "pong kind");

    EXPECT_OK(DecodeReply("+OK\n", &d), "ok");
    expect(d.kind == ReplyKind::kOk, "ok kind");

    EXPECT_OK(DecodeReply("+OK rows_affected=7\n", &d), "ra");
    expect(d.has_rows_affected && d.rows_affected == 7, "ra val");

    EXPECT_OK(DecodeReply("+OK in_txn=1\n", &d), "txn");
    expect(d.has_in_txn && d.in_txn, "in txn");

    EXPECT_OK(DecodeReply("-ERR NotFound: table not found: missing\n", &d), "err");
    expect(d.kind == ReplyKind::kError, "err kind");
    expect_eq(d.error_text, std::string("NotFound: table not found: missing"), "err text");
}

TEST(sql_protocol_statement_terminator) {
    auto& ends = reldb::sqlproto::EndsWithStatementTerminator;
    expect(ends("SELECT 1;"), "simple");
    expect(ends(std::string("SELECT 1;  \n")), "trail ws");
    expect(!ends("SELECT 1"), "no semi");

    // Build cases with explicit chars so quotes are unambiguous.
    const std::string only_in_str = std::string("SELECT ") + "'" + ";" + "'";
    expect(!ends(only_in_str), "semi only in string");

    const std::string then_real = only_in_str + ";";
    expect(ends(then_real), "semi then real semi");

    const std::string empty_lit = std::string("SELECT ") + "'" + "'" + ";";
    expect(ends(empty_lit), "empty string then semi");

    const std::string after = std::string("SELECT ") + "'" + "a" + "'" + ";";
    expect(ends(after), "after string");

    // it''s  -> open, i, t, '', s, close, ;
    const std::string esc = std::string("SELECT ") + "'" + "it" + "'" + "'" + "s" + "'" + ";";
    expect(ends(esc), "escaped quote then semi");

    const std::string mid = std::string("SELECT ") + "'" + "a" + ";" + " b";
    expect(!ends(mid), "semi mid string no");

    expect(ends("SELECT id\nFROM t\nWHERE id=1;"), "multiline");
}

TEST(sql_protocol_try_append_line_cap) {
    std::string buf;
    std::string err;
    expect(TryAppendLine(&buf, "SELECT 1", &err), "append1");
    expect_eq(buf, std::string("SELECT 1"), "buf1");
    expect(TryAppendLine(&buf, "FROM t;", &err), "append2");
    expect_eq(buf, std::string("SELECT 1\nFROM t;"), "buf2");
    expect(EndsWithStatementTerminator(buf), "complete");

    // Cap: fill near limit
    buf.assign(kMaxStatementBytes - 10, 'a');
    err.clear();
    expect(!TryAppendLine(&buf, "0123456789extra", &err), "too large");
    expect(err.find("statement too large") != std::string::npos, "err msg");
    expect(err.find("-ERR ") == 0, "err prefix");
    // buffer unchanged on failure
    expect_eq(buf.size(), kMaxStatementBytes - 10, "buf size");
}

TEST(sql_protocol_appendix_a_select_fixture) {
    // Appendix A happy-path SELECT reply (without *PLAN).
    const std::string wire =
        "*RESULT 1 2\n"
        "*COLS 2\n"
        "id\n"
        "name\n"
        "*ROW\n"
        "$I:1\n"
        "$S:ada\n"
        "*END\n";
    DecodedReply d;
    EXPECT_OK(DecodeReply(wire, &d), "decode");
    expect_eq(d.result.rows[0].at(0).GetInt64(), static_cast<std::int64_t>(1), "id");
    expect_eq(d.result.rows[0].at(1).GetString(), std::string("ada"), "name");
}
