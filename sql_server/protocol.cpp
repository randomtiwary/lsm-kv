#include "protocol.h"

#include <charconv>

#include "reldb/macros.h"
#include "reldb/row.h"

namespace reldb {
namespace sqlproto {
namespace {

void AppendLine(std::string* out, std::string_view line) {
    out->append(line.data(), line.size());
    out->push_back('\n');
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Encode string body after "$S:" with C-style escapes.
std::string EscapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20 || ch == 0x7F) {
                    static const char* kHex = "0123456789ABCDEF";
                    out += "\\x";
                    out.push_back(kHex[ch >> 4]);
                    out.push_back(kHex[ch & 0xF]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

lsmkv::Status UnescapeString(std::string_view escaped, std::string* out) {
    out->clear();
    out->reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        const char ch = escaped[i];
        if (ch != '\\') {
            out->push_back(ch);
            continue;
        }
        if (i + 1 >= escaped.size()) {
            return STATUS(InvalidArgument, "truncated string escape");
        }
        const char e = escaped[++i];
        switch (e) {
            case '\\':
                out->push_back('\\');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'x': {
                if (i + 2 >= escaped.size()) {
                    return STATUS(InvalidArgument, "truncated \\x escape");
                }
                const int hi = HexValue(escaped[i + 1]);
                const int lo = HexValue(escaped[i + 2]);
                if (hi < 0 || lo < 0) {
                    return STATUS(InvalidArgument, "invalid \\x escape");
                }
                out->push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                break;
            }
            default:
                return STATUS(InvalidArgument, "unknown string escape");
        }
    }
    return STATUS(OK);
}

// Split text into lines without trailing '\n'. Keeps empty lines.
std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::string_view line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            lines.push_back(line);
            start = i + 1;
        }
    }
    if (start < text.size()) {
        std::string_view line = text.substr(start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
    } else if (start == text.size() && !text.empty() && text.back() == '\n') {
        // trailing newline only — already consumed
    }
    return lines;
}

bool ParseU64(std::string_view s, std::uint64_t* out) {
    if (s.empty() || out == nullptr) return false;
    std::uint64_t v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last) return false;
    *out = v;
    return true;
}

bool ParseI64(std::string_view s, std::int64_t* out) {
    if (s.empty() || out == nullptr) return false;
    std::int64_t v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr != last) return false;
    *out = v;
    return true;
}

}  // namespace

std::string EncodeCell(const Value& v) {
    switch (v.type()) {
        case ColumnType::kNull:
            return "$N";
        case ColumnType::kInt64:
            return "$I:" + std::to_string(v.GetInt64());
        case ColumnType::kBool:
            return v.GetBool() ? "$B:1" : "$B:0";
        case ColumnType::kString:
            return "$S:" + EscapeString(v.GetString());
    }
    return "$N";
}

lsmkv::Status DecodeCell(std::string_view line, Value* out) {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    if (line == "$N") {
        *out = Value::Null();
        return STATUS(OK);
    }
    if (line.size() >= 3 && line[0] == '$' && line[1] == 'I' && line[2] == ':') {
        std::int64_t v = 0;
        if (!ParseI64(line.substr(3), &v)) {
            return STATUS(InvalidArgument, "bad int cell");
        }
        *out = Value::Int64(v);
        return STATUS(OK);
    }
    if (line == "$B:0") {
        *out = Value::Bool(false);
        return STATUS(OK);
    }
    if (line == "$B:1") {
        *out = Value::Bool(true);
        return STATUS(OK);
    }
    if (line.size() >= 3 && line[0] == '$' && line[1] == 'S' && line[2] == ':') {
        std::string s;
        RELDB_RETURN_NOT_OK(UnescapeString(line.substr(3), &s));
        *out = Value::String(std::move(s));
        return STATUS(OK);
    }
    return STATUS(InvalidArgument, "unknown cell form");
}

std::string EncodeError(const lsmkv::Status& st) {
    std::string out = "-ERR ";
    out += st.ToString();
    out.push_back('\n');
    return out;
}

std::string EncodeOk() { return "+OK\n"; }

std::string EncodeOkRowsAffected(std::uint64_t n) {
    return "+OK rows_affected=" + std::to_string(n) + "\n";
}

std::string EncodePong() { return "+PONG\n"; }

std::string EncodeStatus(bool in_txn) {
    return std::string("+OK in_txn=") + (in_txn ? "1" : "0") + "\n";
}

std::string EncodeQueryResult(const QueryResult& result) {
    const bool is_result_set = !result.column_names.empty() || !result.rows.empty();
    if (!is_result_set) {
        if (result.rows_affected > 0) {
            return EncodeOkRowsAffected(result.rows_affected);
        }
        return EncodeOk();
    }

    std::string out;
    if (!result.plan_tag.empty()) {
        AppendLine(&out, "*PLAN " + result.plan_tag);
    }
    const std::size_t nrows = result.rows.size();
    const std::size_t ncols = result.column_names.empty()
                                  ? (result.rows.empty() ? 0 : result.rows[0].size())
                                  : result.column_names.size();
    AppendLine(&out, "*RESULT " + std::to_string(nrows) + " " + std::to_string(ncols));
    AppendLine(&out, "*COLS " + std::to_string(ncols));
    for (std::size_t c = 0; c < ncols; ++c) {
        if (c < result.column_names.size()) {
            AppendLine(&out, result.column_names[c]);
        } else {
            AppendLine(&out, "");
        }
    }
    for (const auto& row : result.rows) {
        AppendLine(&out, "*ROW");
        for (std::size_t c = 0; c < ncols; ++c) {
            if (c < row.size()) {
                AppendLine(&out, EncodeCell(row.at(c)));
            } else {
                AppendLine(&out, EncodeCell(Value::Null()));
            }
        }
    }
    AppendLine(&out, "*END");
    return out;
}

bool EndsWithStatementTerminator(std::string_view s) {
    bool in_string = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (in_string) {
            if (ch == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') {
                    ++i;  // escaped quote ''
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (ch == '\'') {
            in_string = true;
            continue;
        }
        if (ch == ';') {
            std::size_t j = i + 1;
            while (j < s.size() &&
                   (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) {
                ++j;
            }
            if (j == s.size()) return true;
        }
    }
    return false;
}

bool TryAppendLine(std::string* buffer, std::string_view line, std::string* err_reply) {
    if (buffer == nullptr) return false;
    const std::size_t extra = buffer->empty() ? line.size() : (1 + line.size());
    if (buffer->size() + extra > kMaxStatementBytes) {
        if (err_reply != nullptr) {
            *err_reply =
                EncodeError(lsmkv::Status::InvalidArgument("statement too large"));
        }
        return false;
    }
    if (!buffer->empty()) buffer->push_back('\n');
    buffer->append(line.data(), line.size());
    return true;
}

lsmkv::Status DecodeReply(std::string_view text, DecodedReply* out) {
    if (out == nullptr) {
        return STATUS(InvalidArgument, "null out");
    }
    *out = DecodedReply{};
    auto lines = SplitLines(text);
    if (lines.empty()) {
        return STATUS(InvalidArgument, "empty reply");
    }

    const std::string_view first = lines[0];
    if (first == "+PONG") {
        out->kind = ReplyKind::kPong;
        return STATUS(OK);
    }
    if (first.size() >= 4 && first.substr(0, 4) == "-ERR") {
        out->kind = ReplyKind::kError;
        if (first.size() >= 5 && first[4] == ' ') {
            out->error_text.assign(first.substr(5));
        } else {
            out->error_text.assign(first.substr(4));
        }
        return STATUS(OK);
    }
    if (first.size() >= 3 && first.substr(0, 3) == "+OK") {
        out->kind = ReplyKind::kOk;
        if (first.size() == 3) {
            return STATUS(OK);
        }
        if (first.size() > 4 && first[3] == ' ') {
            const std::string_view rest = first.substr(4);
            constexpr std::string_view kRa = "rows_affected=";
            constexpr std::string_view kTxn = "in_txn=";
            if (rest.size() >= kRa.size() && rest.substr(0, kRa.size()) == kRa) {
                std::uint64_t n = 0;
                if (!ParseU64(rest.substr(kRa.size()), &n)) {
                    return STATUS(InvalidArgument, "bad rows_affected");
                }
                out->has_rows_affected = true;
                out->rows_affected = n;
                return STATUS(OK);
            }
            if (rest.size() >= kTxn.size() && rest.substr(0, kTxn.size()) == kTxn) {
                if (rest.substr(kTxn.size()) == "0") {
                    out->has_in_txn = true;
                    out->in_txn = false;
                    return STATUS(OK);
                }
                if (rest.substr(kTxn.size()) == "1") {
                    out->has_in_txn = true;
                    out->in_txn = true;
                    return STATUS(OK);
                }
                return STATUS(InvalidArgument, "bad in_txn");
            }
        }
        return STATUS(OK);
    }

    // Result block
    std::size_t i = 0;
    if (i < lines.size() && lines[i].size() >= 6 && lines[i].substr(0, 6) == "*PLAN ") {
        out->plan_tag.assign(lines[i].substr(6));
        ++i;
    }
    if (i >= lines.size() || lines[i].size() < 8 || lines[i].substr(0, 8) != "*RESULT ") {
        out->kind = ReplyKind::kUnknown;
        return STATUS(InvalidArgument, "expected *RESULT");
    }
    {
        std::string_view rest = lines[i].substr(8);
        const auto sp = rest.find(' ');
        if (sp == std::string_view::npos) {
            return STATUS(InvalidArgument, "bad *RESULT");
        }
        std::uint64_t nrows = 0;
        std::uint64_t ncols = 0;
        if (!ParseU64(rest.substr(0, sp), &nrows) || !ParseU64(rest.substr(sp + 1), &ncols)) {
            return STATUS(InvalidArgument, "bad *RESULT counts");
        }
        ++i;
        if (i >= lines.size() || lines[i].size() < 6 || lines[i].substr(0, 6) != "*COLS ") {
            return STATUS(InvalidArgument, "expected *COLS");
        }
        std::uint64_t ncols2 = 0;
        if (!ParseU64(lines[i].substr(6), &ncols2) || ncols2 != ncols) {
            return STATUS(InvalidArgument, "*COLS mismatch");
        }
        ++i;
        out->result.column_names.clear();
        out->result.column_names.reserve(static_cast<std::size_t>(ncols));
        for (std::uint64_t c = 0; c < ncols; ++c) {
            if (i >= lines.size()) {
                return STATUS(InvalidArgument, "missing column name");
            }
            out->result.column_names.emplace_back(lines[i]);
            ++i;
        }
        out->result.rows.clear();
        out->result.rows.reserve(static_cast<std::size_t>(nrows));
        for (std::uint64_t r = 0; r < nrows; ++r) {
            if (i >= lines.size() || lines[i] != "*ROW") {
                return STATUS(InvalidArgument, "expected *ROW");
            }
            ++i;
            std::vector<Value> cells;
            cells.reserve(static_cast<std::size_t>(ncols));
            for (std::uint64_t c = 0; c < ncols; ++c) {
                if (i >= lines.size()) {
                    return STATUS(InvalidArgument, "missing cell");
                }
                Value cell;
                RELDB_RETURN_NOT_OK(DecodeCell(lines[i], &cell));
                cells.push_back(std::move(cell));
                ++i;
            }
            out->result.rows.emplace_back(std::move(cells));
        }
        if (i >= lines.size() || lines[i] != "*END") {
            return STATUS(InvalidArgument, "expected *END");
        }
        out->result.plan_tag = out->plan_tag;
        out->kind = ReplyKind::kResult;
        return STATUS(OK);
    }
}

}  // namespace sqlproto
}  // namespace reldb
