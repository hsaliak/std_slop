#include "completion.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cstring>

using json = nlohmann::json;

namespace slop {

static json global_commands;

// Helper to tokenize the current line
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string token;
    while (ss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Logic separated for testing
std::vector<std::string> GetCompletionMatches(const std::string& text, 
                                               const std::string& line_until_cursor,
                                               const json& command_tree) {
    std::vector<std::string> matches;
    std::vector<std::string> tokens = Tokenize(line_until_cursor);
    
    bool has_trailing_space = (!line_until_cursor.empty() && line_until_cursor.back() == ' ');
    
    const json* current_node = &command_tree;
    size_t token_count = tokens.size();
    size_t depth = has_trailing_space ? token_count : (token_count > 0 ? token_count - 1 : 0);
    
    for (size_t i = 0; i < depth; ++i) {
        if (current_node->is_object() && current_node->contains(tokens[i])) {
            current_node = &((*current_node)[tokens[i]]);
        } else {
            return {};
        }
    }

    if (current_node->is_object()) {
        for (auto it = current_node->begin(); it != current_node->end(); ++it) {
            if (it.key().compare(0, text.size(), text) == 0) {
                matches.push_back(it.key());
            }
        }
    }
    return matches;
}

// The generator function
char* command_generator(const char* text, int state) {
    static std::vector<std::string> matches;
    static size_t match_index;

    if (!state) {
        matches.clear();
        match_index = 0;

        std::string line_buffer(rl_line_buffer);
        std::string line_until_cursor = line_buffer.substr(0, static_cast<size_t>(rl_point));
        
        matches = GetCompletionMatches(text, line_until_cursor, global_commands);
    }

    if (match_index < matches.size()) {
        return strdup(matches[match_index++].c_str());
    }

    return nullptr;
}

char** slop_completion(const char* text, int start, int end) {
    if (rl_line_buffer && rl_line_buffer[0] == '/') {
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, command_generator);
    }
    return nullptr;
}

void InitCompletion(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.is_open()) return;
    
    global_commands = json::parse(f, nullptr, false);
    if (global_commands.is_discarded()) {
        global_commands = json::object();
        return;
    }

    rl_attempted_completion_function = slop_completion;
    rl_completer_word_break_characters = (char*)" \t\n\"\\'`@$><=;|&{(";
}

} // namespace slop
