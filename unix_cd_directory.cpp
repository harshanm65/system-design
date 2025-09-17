#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <stdexcept>

using namespace std;

class PathResolver {
private:
    unordered_map<string, string> symlinks;

    /**
     * @brief Splits a string by a delimiter into a vector of tokens.
     * Handles redundant delimiters.
     */
    vector<string> split(const string& s, char delimiter) {
        vector<string> tokens;
        string token;
        istringstream tokenStream(s);
        while (getline(tokenStream, token, delimiter)) {
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        return tokens;
    }

    /**
     * @brief Joins a vector of strings into a single string with a delimiter.
     */
    string join(const vector<string>& elements, const string& delimiter) {
        stringstream ss;
        for (size_t i = 0; i < elements.size(); ++i) {
            ss << elements[i];
            if (i < elements.size() - 1) {
                ss << delimiter;
            }
        }
        return ss.str();
    }

    /**
     * @brief Resolves a symbolic link path using DFS, detecting cycles.
     * Throws a runtime_error if a cycle is detected.
     */
    string resolve_symlinks_dfs(const string& path, unordered_set<string>& visited) {
        // Cycle detected
        if (visited.count(path)) {
            throw runtime_error("Symbolic link cycle detected at '" + path + "'");
        }

        if (symlinks.count(path)) {
            visited.insert(path);
            string target = symlinks[path];
            return resolve_symlinks_dfs(target, visited);
        }

        // Base case: path is not a symlink
        return path;
    }
    string get_longest_symlink_match(const string& current_path) const {
        string best = "";
        for (const auto& kv : symlinks) {
            const string& key = kv.first;
            if (current_path.compare(0, key.size(), key) == 0 &&
                key.size() > best.size()) {
                best = key;
            }
        }
        return best;
    }

public:
    PathResolver(const unordered_map<string, string>& symlink_map)
        : symlinks(symlink_map) {}

    /**
     * @brief Resolves a target path relative to a current working directory (cwd).
     */
    string resolve(const string& cwd, const string& target_path) {
        vector<string> path_stack;

        // Initialize stack based on whether the path is absolute or relative
        if (target_path[0] == '/') {
            path_stack = {};
        } else {
            path_stack = split(cwd, '/');
        }

        vector<string> components_to_process = split(target_path, '/');

        size_t i = 0;
        while (i < components_to_process.size()) {
            string component = components_to_process[i];

            if (component == ".") {
                i++;
                continue;
            }
            if (component == "..") {
                if (!path_stack.empty()) {
                    path_stack.pop_back();
                }
                i++;
                continue;
            }

            // Construct the path to check for a symlink
            string current_path = "/" + join(path_stack, "/") + "/" + component;

            string matched_symlink = get_longest_symlink_match(current_path);
            if (!matched_symlink.empty()) {
                try {
                    unordered_set<string> visited;
                    string target = resolve_symlinks_dfs(matched_symlink, visited);

                    vector<string> remaining_components;
                    for(size_t j = i + 1; j < components_to_process.size(); ++j) {
                        remaining_components.push_back(components_to_process[j]);
                    }

                    string new_path_str = target;
                    if (!remaining_components.empty()) {
                        if (new_path_str.back() != '/') {
                           new_path_str += "/";
                        }
                        new_path_str += join(remaining_components, "/");
                    }

                    if (target[0] == '/') { // target.starts_with('/')
                        path_stack.clear();
                    }

                    components_to_process = split(new_path_str, '/');
                    i = 0; // Restart processing with new components
                    continue;

                } catch (const runtime_error& e) {
                    cerr << "Error: " << e.what() << endl;
                    return cwd; // Return original CWD on error
                }
            }

            path_stack.push_back(component);
            i++;
        }

        if (path_stack.empty()) {
            return "/";
        }

        return "/" + join(path_stack, "/");
    }
};

// --- Demonstration ---
int main() {
    // 1. Define the symbolic link structure
    unordered_map<string, string> symlinks = {
        {"/a", "/x"},
        {"/a/b/c", "/y"},
        {"/home/user/dox", "/home/user/documents"},
        {"/home/user/latest-log", "logs/today.log"},
        {"/usr/data", "/var/data"},
        {"/cycle1", "/cycle2"},
        {"/cycle2", "/home/user/../cycle1"},
        // Removed duplicate symlink entry

    };

    PathResolver resolver(symlinks);

    // 2. Run test cases
    vector<pair<string, string>> test_cases = {
        {"/", "a/b/c/d"},
        {"/", "home/user"},
        {"/home/user", ".."},
        {"/home/user/documents", "../.././usr/../home"},
        {"/home", "user/dox/reports"},
        {"/home/user", "latest-log"},
        {"/var", "../usr/data/files"},
        {"/", "cycle1/some/path"},
        {"/", "/home/user/dox/../dox/."}
    };

    cout << "--- Path Resolution Simulation ---" << endl;
    for (const auto& test_case : test_cases) {
        string result = resolver.resolve(test_case.first, test_case.second);
        cout << "cwd: '" << test_case.first << "' ; cd '" << test_case.second
                  << "'  -->  new cwd: '" << result << "'" << endl;
    }

    return 0;
}
