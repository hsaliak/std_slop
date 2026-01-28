#include "markdown/renderer.h"
#include "interface/color.h"
#include <string_view>
#include <iostream>
#include <vector>
#include <algorithm>

namespace slop {
namespace markdown {

namespace {
std::string_view GetNodeText(TSNode node, std::string_view source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.length()) return "";
    uint32_t len = std::min(end - start, (uint32_t)(source.length() - start));
    return source.substr(start, len);
}

struct Style {
    const char* pre;
    const char* post;
};

size_t DisplayWidth(std::string_view text) {
    size_t width = 0;
    bool in_esc = false;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '\033') {
            in_esc = true;
        } else if (in_esc) {
            if ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')) {
                in_esc = false;
            }
        } else {
            // Very basic UTF-8 support: only count start bytes
            if ((text[i] & 0xc0) != 0x80) {
                width++;
            }
        }
    }
    return width;
}

Style GetNodeStyle(std::string_view type) {
    using namespace ansi::theme::markdown;
    if (type == "atx_heading") return {Header, ansi::Reset};
    if (type == "atx_h1_marker" || type == "atx_h2_marker" || type == "atx_h3_marker") return {HeaderMarker, ""};
    if (type == "strong_emphasis") return {Bold, ansi::Reset};
    if (type == "emphasis") return {Italic, ansi::Reset};
    if (type == "fenced_code_block") return {CodeBlock, ansi::Reset};
    if (type == "code_span" || type == "code_inline" || type == "inline_code") return {CodeInline, ansi::Reset};
    if (type == "link_destination") return {LinkUrl, ansi::Reset};
    if (type == "link_text") return {LinkText, ansi::Reset};
    if (type == "list_marker" || type.find("list_marker_") == 0) return {ListMarker, ansi::Reset};
    if (type == "block_quote_marker") return {Quote, ansi::Reset};
    if (type == "thematic_break") return {HorizontalRule, ansi::Reset};
    return {"", ""};
}
}

std::string MarkdownRenderer::Render(const ParsedMarkdown& parsed) {
    std::string output;
    output.reserve(parsed.source().length() * 1.2); // Pre-allocate with some slack for ANSI codes
    TSNode root = ts_tree_root_node(parsed.tree());
    RenderNodeRecursive(root, parsed, parsed.source(), output, 0, parsed.tree());
    return output;
}

void MarkdownRenderer::RenderNodeRecursive(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source, std::string& output, int depth, TSTree* current_tree) {
    if (depth > 128) return; // Safety

    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    
    // Check for injections (only if we are in the main tree)
    if (current_tree == parsed.tree()) {
        if (const auto* inj = parsed.GetInjection({start, end})) {
            if (inj->tree) {
                RenderNodeRecursive(ts_tree_root_node(inj->tree.get()), parsed, GetNodeText(node, current_source), output, depth + 1, inj->tree.get());
            } else {
                output.append(GetNodeText(node, current_source));
            }
            return;
        }
    }

    std::string_view type = ts_node_type(node);
    if (type == "pipe_table") {
        RenderTable(node, parsed, current_source, output, depth, current_tree);
        return;
    }


    Style style = GetNodeStyle(type);

    output.append(style.pre);

    uint32_t child_count = ts_node_child_count(node);
    if (child_count == 0) {
        output.append(GetNodeText(node, current_source));
    } else {
        uint32_t last_pos = start;
        for (uint32_t i = 0; i < child_count; ++i) {
            TSNode child = ts_node_child(node, i);
            uint32_t child_start = ts_node_start_byte(child);

            if (child_start > last_pos) {
                output.append(current_source.substr(last_pos, child_start - last_pos));
            }

            RenderNodeRecursive(child, parsed, current_source, output, depth + 1, current_tree);
            last_pos = ts_node_end_byte(child);

            // Re-apply style if we're in a heading and just finished the marker
            if (type == "atx_heading" && i == 0) {
                output.append(style.pre);
            }
        }
        if (end > last_pos) {
            output.append(current_source.substr(last_pos, end - last_pos));
        }
    }

    output.append(style.post);
}

std::string MarkdownRenderer::RenderCellToText(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source, TSTree* current_tree) {
    std::string result;
    RenderNodeRecursive(node, parsed, current_source, result, 0, current_tree);
    return result;
}

namespace {
std::string Align(std::string text, size_t width, MarkdownRenderer::TableColumn::Alignment align) {
    size_t dw = DisplayWidth(text);
    if (dw >= width) return text;
    size_t extra = width - dw;
    if (align == MarkdownRenderer::TableColumn::LEFT) {
        return text + std::string(extra, ' ');
    } else if (align == MarkdownRenderer::TableColumn::RIGHT) {
        return std::string(extra, ' ') + text;
    } else { // CENTER
        size_t left = extra / 2;
        size_t right = extra - left;
        return std::string(left, ' ') + text + std::string(right, ' ');
    }
}
}

void MarkdownRenderer::RenderTable(TSNode node, const ParsedMarkdown& parsed, std::string_view current_source, std::string& output, int /*depth*/, TSTree* current_tree) {
    std::vector<TableColumn> columns;
    std::vector<std::vector<std::string>> rows;
    
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view type = ts_node_type(child);
        
        if (type == "pipe_table_header" || type == "pipe_table_row") {
            std::vector<std::string> row_cells;
            uint32_t cell_count = ts_node_child_count(child);
            size_t col_idx = 0;
            for (uint32_t j = 0; j < cell_count; ++j) {
                TSNode cell = ts_node_child(child, j);
                if (std::string_view(ts_node_type(cell)) == "pipe_table_cell") {
                    std::string content = RenderCellToText(cell, parsed, current_source, current_tree);
                    // Trim content
                    size_t first = content.find_first_not_of(" \t\r\n");
                    if (first != std::string::npos) {
                        size_t last = content.find_last_not_of(" \t\r\n");
                        content = content.substr(first, (last - first + 1));
                    } else {
                        content = "";
                    }
                    
                    size_t width = DisplayWidth(content);
                    if (col_idx >= columns.size()) {
                        columns.push_back({width, TableColumn::LEFT});
                    } else {
                        columns[col_idx].width = std::max(columns[col_idx].width, width);
                    }
                    row_cells.push_back(std::move(content));
                    col_idx++;
                }
            }
            rows.push_back(std::move(row_cells));
        } else if (type == "pipe_table_delimiter_row") {
            uint32_t cell_count = ts_node_child_count(child);
            size_t col_idx = 0;
            for (uint32_t j = 0; j < cell_count; ++j) {
                TSNode cell = ts_node_child(child, j);
                if (std::string_view(ts_node_type(cell)) == "pipe_table_delimiter_cell") {
                    if (col_idx >= columns.size()) columns.push_back({0, TableColumn::LEFT});
                    
                    bool has_left = false;
                    bool has_right = false;
                    uint32_t sub_count = ts_node_child_count(cell);
                    for (uint32_t k = 0; k < sub_count; ++k) {
                        std::string_view sub_type = ts_node_type(ts_node_child(cell, k));
                        if (sub_type == "pipe_table_align_left") has_left = true;
                        if (sub_type == "pipe_table_align_right") has_right = true;
                    }
                    
                    if (has_left && has_right) columns[col_idx].alignment = TableColumn::CENTER;
                    else if (has_right) columns[col_idx].alignment = TableColumn::RIGHT;
                    else columns[col_idx].alignment = TableColumn::LEFT;
                    
                    col_idx++;
                }
            }
        }
    }

    if (columns.empty()) return;

    using namespace ansi::theme::markdown;
    
    // Top border
    output.append(TableBorder);
    output.append("┌");
    for (size_t i = 0; i < columns.size(); ++i) {
        for (size_t j = 0; j < columns[i].width + 2; ++j) {
            output.append("─");
        }
        if (i < columns.size() - 1) {
            output.append("┬");
        }
    }
    output.append("┐\n");

    for (size_t r = 0; r < rows.size(); ++r) {
        output.append(TableBorder);
        output.append("│");
        for (size_t c = 0; c < columns.size(); ++c) {
            std::string content = (c < rows[r].size()) ? rows[r][c] : "";
            std::string aligned = Align(content, columns[c].width, columns[c].alignment);
            
            output.append(" ");
            if (r == 0) output.append(TableHeader);
            output.append(aligned);
            if (r == 0) output.append(ansi::Reset);
            output.append(" ");
            
            output.append(TableBorder);
            output.append("│");
        }
        output.append(ansi::Reset);
        output.append("\n");

        if (r == 0) { // After header
            output.append(TableBorder);
            output.append("├");
            for (size_t i = 0; i < columns.size(); ++i) {
                for (size_t j = 0; j < columns[i].width + 2; ++j) output.append("─");
                if (i < columns.size() - 1) {
                    output.append("┼");
                }
            }
            output.append("┤\n");
        }
    }

    // Bottom border
    output.append(TableBorder);
    output.append("└");
    for (size_t i = 0; i < columns.size(); ++i) {
        for (size_t j = 0; j < columns[i].width + 2; ++j) output.append("─");
        if (i < columns.size() - 1) {
            output.append("┴");
        }
    }
    output.append("┘\n");
    output.append(ansi::Reset);
}

} // namespace markdown
} // namespace slop
