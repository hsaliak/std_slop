# INI Configuration Implementation Plan

This document outlines the implementation plan for adding INI configuration support to `std_slop`.

## Goals
- Allow users to persist settings in an INI file.
- Support a default config location at `~/.config/slop/config.ini`.
- Ensure a clear precedence order: CLI arguments > INI file > Environment variables > Defaults.
- Implement a clean-room INI parser in a dedicated `ini/` library.

## Architectural Changes

### 1. Utility Refactoring
- Move `GetHomeDir()` from `core/oauth_handler.cpp` to `core/shell_util.h/cpp`.
- This ensures consistent home directory discovery across the codebase.

### 2. Clean Room INI Library (`ini/`)
- **`ini/ini_parser.h`**: Define `IniConfig` class and `ParseIni` function.
- **`ini/ini_parser.cpp`**: Implementation of a simple parser for `[section]` and `key=value` pairs.
- **`ini/BUILD.bazel`**: Define `cc_library`.

### 3. Configuration Module (`core/config.h/cpp`)
- **`slop::LoadConfigAndApply(const std::string& override_path)`**:
    - Resolve the config path (use `override_path` or default to `~/.config/slop/config.ini`).
    - Parse the INI file.
    - Apply values to `absl` flags using `absl::FindCommandLineFlag`.
    - **Crucial**: Only update flags if `!flag->IsSpecifiedOnCommandLine()`.

### 4. Documentation
- **`example_config.ini`**: Provide a template with all supported keys and descriptions.

### 5. Main Integration
- Add `ABSL_FLAG(std::string, config, "", "Path to config INI file");`.
- Call `slop::LoadConfigAndApply()` in `main()` immediately after `absl::ParseCommandLine`.

## Precedence Order
1.  **Command Line Arguments**: Always take highest priority.
2.  **INI File**: Applied only if the flag was not set on the CLI.
3.  **Environment Variables**: (e.g., `GOOGLE_API_KEY`) checked by existing application logic if flags are empty.
4.  **Hardcoded Defaults**: Built-in `absl` flag defaults.

## Exclusive Flags Management
Settings like `google_api_key` vs `openai_api_key` are managed by the existing logic in `core/orchestrator.cpp` and `main.cpp`. The INI loader simply populates the flags; the existing priority logic (OpenAI > Gemini) remains the source of truth for conflict resolution.
