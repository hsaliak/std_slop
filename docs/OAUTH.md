# OAuth and Token Management

This document describes OAuth behavior in `std::slop` for OpenAI.

## Overview

`std::slop` includes built-in OpenAI OAuth device authorization via `std_slop --fetch-oauth`.
The C++ `OAuthHandler` is responsible for:
- loading persisted tokens,
- refreshing access tokens using `refresh_token`,
- initiating device authorization and polling for the first token,
- exposing valid bearer tokens to runtime request paths.

## OpenAI OAuth Quickstart

### OpenAI OAuth (ChatGPT Plus)
- Command: `std_slop --fetch-oauth`
- Runtime command after login: `std_slop --openai_oauth`
- Token file: `~/.config/slop/chatgpt_plus_token.json`
- Runtime flag: `--openai_oauth`
- API mode: OpenAI Responses API (forced)
- Runtime base URL: `https://chatgpt.com/backend-api/codex` (custom `openai_base_url` is ignored in this mode)

## Runtime Selection Rules

- OpenAI API key mode uses Chat Completions by default.
- OpenAI API key mode can switch to Responses with `--use_responses`.

## Token Refresh Behavior

`OAuthHandler` automatically refreshes tokens before expiry:
- OpenAI refresh endpoint: `https://auth.openai.com/oauth/token`

If refresh fails or tokens are missing:
- OpenAI guidance: run `std_slop --fetch-oauth`

## OpenAI OAuth Token Path Override

You can override the default OpenAI token path with:
- CLI: `--openai_oauth_token_path=/custom/path/chatgpt_plus_token.json`
- INI key: `openai_oauth_token_path = /custom/path/chatgpt_plus_token.json`

## Test Coverage

`oauth_handler_test.cpp` covers:
- default token path selection for OpenAI OAuth provider,
- token loading behavior,

