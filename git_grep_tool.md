`git grep` is one of the most powerful yet underutilized tools in the Git toolkit. While the standard Unix `grep` searches your file system, `git grep` is specifically optimized to search through your project's history, specific branches, and tracked files with incredible speed.

The biggest advantage? It **ignores** anything in your `.gitignore` and doesn't waste time searching inside the `.git` directory itself.

---

## 1. The Basics: Searching the Current Working Directory

By default, `git grep` looks for a string in all tracked files in your current directory and subdirectories.

* **Simple search:** `git grep "search_term"`
* **Case-insensitive search:**
`git grep -i "search_term"`
* **Show line numbers:**
`git grep -n "search_term"`
* **Count occurrences per file:**
`git grep -c "search_term"`

---

## 2. Searching Beyond the Working Tree

Unlike standard `grep`, `git grep` can look through your project's history without you having to check out old branches.

### Search a Specific Branch or Tag

To see if a function existed in an old version or a different feature branch:
`git grep "function_name" branch_name`

### Search All History

If you want to search every commit you've ever made (this can be slow on massive repos):
`git grep "secret_key" $(git rev-list --all)`

### Search the Staging Area (Index)

To search only files that you have staged for the next commit:
`git grep --cached "search_term"`

---

## 3. Formatting and Context

When searching for code, seeing the line *around* the match is often more important than the match itself.

| Flag | Purpose | Example |
| --- | --- | --- |
| `-A <n>` | Show **After**: lines after the match | `git grep -A 3 "init"` |
| `-B <n>` | Show **Before**: lines before the match | `git grep -B 2 "init"` |
| `-C <n>` | Show **Context**: lines before and after | `git grep -C 5 "init"` |
| `-p` | Show **Function**: shows which function/method the match is in | `git grep -p "TODO"` |
| `-l` | **Files only**: just lists names of files with matches | `git grep -l "TODO"` |

---

## 4. Advanced Pattern Matching (Boolean Logic)

`git grep` supports complex logic that allows you to find files that contain multiple specific patterns.

### The "AND" Search

To find lines that contain "Error" **and** "Critical" in the same file:
`git grep -e "Error" --and -e "Critical"`

### Finding Multiple Patterns (OR)

To find lines that contain either "Fix" **or** "Bug":
`git grep -e "Fix" -e "Bug"`

### The "All-Match" Flag

If you want to find files that contain "Pattern A" somewhere and "Pattern B" somewhere else (not necessarily on the same line), use `--all-match`:
`git grep --all-match -e "import react" -e "useContext"`

---

## 5. Filtering by File Type (Pathspecs)

You can limit your search to specific folders or file extensions. This is significantly faster in large monorepos.

* **Search only JavaScript files:**
`git grep "api_key" -- "*.js"`
* **Search only in the `src` folder:**
`git grep "api_key" -- src/`
* **Exclude a directory:**
`git grep "api_key" -- ":(exclude)tests/"`

---

## 6. Useful Pro-Tips

### Use "Word" Boundaries

If you search for `is`, you'll get hits for `this`, `island`, and `finish`. To find the exact word `is` only:
`git grep -w "is"`

### Search for Regular Expressions

`git grep` supports Basic POSIX expressions by default, but you can use Perl-compatible regular expressions (PCRE) for more power:
`git grep -P "(\d{3})-\d{3}-\d{4}"` *(Finds phone numbers)*

### Configure it permanently

If you always want line numbers, you can set it in your global config:
`git config --global grep.lineNumber true`

---

> **Note:** If you find yourself searching for a string that you previously deleted and it's no longer in your current branch, `git grep` won't find it unless you specify the commit hash where it existed. For searching *deleted* content specifically, `git log -S "string"` (the "pickaxe") is actually the better tool.

