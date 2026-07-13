#include "reldb/query_format.h"

#include <string>
#include <vector>

#include "reldb/types.h"

namespace reldb {

void FormatQueryResult(std::ostream& out, const QueryResult& r) {
    if (!r.plan_tag.empty()) {
        out << "plan: " << r.plan_tag << "\n";
    }
    if (r.rows_affected != 0 && r.rows.empty()) {
        out << "rows_affected: " << r.rows_affected << "\n";
        return;
    }
    if (r.column_names.empty() && r.rows.empty()) {
        out << "ok\n";
        return;
    }

    std::vector<std::size_t> widths(r.column_names.size(), 0);
    for (std::size_t c = 0; c < r.column_names.size(); ++c) {
        widths[c] = r.column_names[c].size();
    }
    std::vector<std::vector<std::string>> cells;
    cells.reserve(r.rows.size());
    for (const auto& row : r.rows) {
        std::vector<std::string> line;
        line.reserve(row.size());
        for (std::size_t c = 0; c < row.size(); ++c) {
            std::string s = row.at(c).ToString();
            if (c < widths.size() && s.size() > widths[c]) widths[c] = s.size();
            line.push_back(std::move(s));
        }
        cells.push_back(std::move(line));
    }

    auto print_sep = [&]() {
        out << "+";
        for (std::size_t w : widths) {
            out << std::string(w + 2, '-') << "+";
        }
        out << "\n";
    };

    if (!r.column_names.empty()) {
        print_sep();
        out << "|";
        for (std::size_t c = 0; c < r.column_names.size(); ++c) {
            out << " " << r.column_names[c]
                << std::string(widths[c] - r.column_names[c].size(), ' ') << " |";
        }
        out << "\n";
        print_sep();
    }
    for (const auto& line : cells) {
        out << "|";
        for (std::size_t c = 0; c < line.size(); ++c) {
            const std::size_t w = c < widths.size() ? widths[c] : line[c].size();
            out << " " << line[c] << std::string(w - line[c].size(), ' ') << " |";
        }
        out << "\n";
    }
    if (!r.column_names.empty()) print_sep();
    out << "(" << r.rows.size() << " row" << (r.rows.size() == 1 ? "" : "s") << ")\n";
    if (r.rows_affected != 0) {
        out << "rows_affected: " << r.rows_affected << "\n";
    }
}

}  // namespace reldb
