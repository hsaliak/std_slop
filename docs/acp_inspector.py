#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from dataclasses import dataclass
from typing import Any, Optional


class InspectorError(Exception):
    pass


@dataclass
class ServerResponse:
    raw: str
    parsed: Optional[dict[str, Any]]


class ACPInspector:
    def __init__(self, command: list[str], session_id: str = "default_session") -> None:
        self.command = command
        self.session_id = session_id
        self.next_id = 1
        self.proc: Optional[subprocess.Popen[str]] = None
        self.initialized = False

    def start(self) -> None:
        if self.proc is not None:
            raise InspectorError("Server is already running")

        try:
            self.proc = subprocess.Popen(
                self.command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
        except OSError as exc:
            raise InspectorError(f"Failed to start server: {exc}") from exc

    def stop(self) -> None:
        if self.proc is None:
            return

        try:
            if self.proc.stdin and not self.proc.stdin.closed:
                self.proc.stdin.close()
        except Exception:
            pass

        try:
            self.proc.terminate()
            self.proc.wait(timeout=1.0)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        finally:
            self.proc = None

    def _ensure_running(self) -> subprocess.Popen[str]:
        if self.proc is None:
            raise InspectorError("Server is not running")
        if self.proc.stdin is None or self.proc.stdout is None:
            raise InspectorError("Server pipes are unavailable")
        return self.proc

    def _next_id(self) -> int:
        request_id = self.next_id
        self.next_id += 1
        return request_id

    def send_request(self, method: str, params: Optional[dict[str, Any]] = None, *, request_id: Optional[int] = None) -> ServerResponse:
        proc = self._ensure_running()
        actual_request_id = request_id if request_id is not None else self._next_id()
        req = {
            "jsonrpc": "2.0",
            "id": actual_request_id,
            "method": method,
            "params": params or {},
        }
        payload = json.dumps(req, separators=(",", ":"))

        assert proc.stdin is not None
        assert proc.stdout is not None
        proc.stdin.write(payload + "\n")
        proc.stdin.flush()

        while True:
            line = proc.stdout.readline()
            if not line:
                stderr_text = ""
                if proc.stderr is not None:
                    try:
                        stderr_text = proc.stderr.read()
                    except Exception:
                        stderr_text = ""
                raise InspectorError(
                    "Server closed stdout without returning a response"
                    + (f". stderr: {stderr_text}" if stderr_text else "")
                )

            raw = line.rstrip("\n")
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError:
                parsed = None

            # ACP servers may emit notifications or progress messages before the
            # final response to our request. Show those immediately and keep waiting.
            if isinstance(parsed, dict) and parsed.get("id") == actual_request_id:
                return ServerResponse(raw=raw, parsed=parsed)

            print("[server]", end=" ")
            if parsed is None:
                print(raw)
            else:
                print(json.dumps(parsed, indent=2, sort_keys=True))

    def initialize(self) -> ServerResponse:
        response = self.send_request(
            "initialize",
            {"protocolVersion": "1", "capabilities": {}},
        )
        if response.parsed and "result" in response.parsed:
            self.initialized = True
        return response

    def session_new(self) -> ServerResponse:
        response = self.send_request("session/new", {})
        if response.parsed and isinstance(response.parsed, dict):
            result = response.parsed.get("result")
            if isinstance(result, dict):
                for key in ("sessionId", "session", "id"):
                    value = result.get(key)
                    if isinstance(value, str) and value:
                        self.session_id = value
                        break
        return response

    def session_prompt(self, prompt: str, *, session_id: Optional[str] = None) -> ServerResponse:
        return self.send_request(
            "session/prompt",
            {"sessionId": session_id or self.session_id, "prompt": prompt},
        )

    def session_cancel(self, *, session_id: Optional[str] = None) -> ServerResponse:
        return self.send_request(
            "session/cancel",
            {"sessionId": session_id or self.session_id},
        )

    def raw(self, payload: str) -> ServerResponse:
        proc = self._ensure_running()
        assert proc.stdin is not None
        assert proc.stdout is not None
        proc.stdin.write(payload.rstrip("\n") + "\n")
        proc.stdin.flush()

        expected_id = None
        try:
            sent = json.loads(payload)
            if isinstance(sent, dict):
                expected_id = sent.get("id")
        except json.JSONDecodeError:
            sent = None

        while True:
            line = proc.stdout.readline()
            if not line:
                raise InspectorError("Server closed stdout without returning a response")
            raw = line.rstrip("\n")
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError:
                parsed = None

            if expected_id is None:
                return ServerResponse(raw=raw, parsed=parsed)
            if isinstance(parsed, dict) and parsed.get("id") == expected_id:
                return ServerResponse(raw=raw, parsed=parsed)

            print("[server]", end=" ")
            if parsed is None:
                print(raw)
            else:
                print(json.dumps(parsed, indent=2, sort_keys=True))

    def stderr_drain(self) -> str:
        if self.proc is None or self.proc.stderr is None:
            return ""
        try:
            import os
            import select

            fd = self.proc.stderr.fileno()
            ready, _, _ = select.select([fd], [], [], 0)
            if not ready:
                return ""
            chunks: list[str] = []
            while True:
                ready, _, _ = select.select([fd], [], [], 0)
                if not ready:
                    break
                chunk = os.read(fd, 4096).decode(errors="replace")
                if not chunk:
                    break
                chunks.append(chunk)
            return "".join(chunks)
        except Exception:
            return ""


def print_response(resp: ServerResponse) -> None:
    if resp.parsed is None:
        print(resp.raw)
        return
    print(json.dumps(resp.parsed, indent=2, sort_keys=True))


def print_help() -> None:
    print(
        """
Commands:
  help                         Show this help
  init                         Send initialize
  new                          Send session/new
  prompt <text>                Send session/prompt using current sessionId
  cancel                       Send session/cancel using current sessionId
  session <id>                 Set local sessionId without calling server
  raw <json>                   Send an exact JSON line as-is
  id                           Show current local sessionId
  stderr                       Drain and print any available stderr output
  quit / exit                  Exit the inspector
""".strip()
    )


def run_repl(inspector: ACPInspector) -> int:
    print(f"Using local session id: {inspector.session_id}")
    print("Type 'help' for commands.")

    while True:
        try:
            line = input("acp> ").strip()
        except EOFError:
            print()
            return 0
        except KeyboardInterrupt:
            print()
            return 0

        if not line:
            continue

        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"Parse error: {exc}")
            continue

        cmd = parts[0].lower()

        try:
            if cmd in {"quit", "exit"}:
                return 0
            if cmd == "help":
                print_help()
            elif cmd == "init":
                print_response(inspector.initialize())
            elif cmd == "new":
                print_response(inspector.session_new())
                print(f"Current session id: {inspector.session_id}")
            elif cmd == "prompt":
                if len(parts) < 2:
                    print("Usage: prompt <text>")
                    continue
                text = line[len(parts[0]) :].strip()
                print_response(inspector.session_prompt(text))
            elif cmd == "cancel":
                print_response(inspector.session_cancel())
            elif cmd == "session":
                if len(parts) != 2:
                    print("Usage: session <id>")
                    continue
                inspector.session_id = parts[1]
                print(f"Current session id: {inspector.session_id}")
            elif cmd == "id":
                print(inspector.session_id)
            elif cmd == "raw":
                payload = line[len(parts[0]) :].strip()
                if not payload:
                    print("Usage: raw <json>")
                    continue
                print_response(inspector.raw(payload))
            elif cmd == "stderr":
                data = inspector.stderr_drain()
                print(data if data else "<no stderr output available>")
            else:
                print(f"Unknown command: {cmd}")
        except InspectorError as exc:
            print(f"Error: {exc}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Interactive ACP stdio inspector for newline-delimited JSON-RPC servers.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "server",
        nargs=argparse.REMAINDER,
        help="Server command to run, for example: ./your_server --flag value",
    )
    parser.add_argument(
        "--session-id",
        default="default_session",
        help="Initial local session id to use for prompt/cancel commands",
    )
    parser.add_argument(
        "--no-auto-start",
        action="store_true",
        help="Parse arguments and exit without launching the server",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    command = args.server
    if command and command[0] == "--":
        command = command[1:]

    if not command:
        print(
            "Provide the server command after '--'. Example:\n"
            "  python acp_inspector.py -- ./your_server\n"
            "  python acp_inspector.py -- cargo run --bin your_server",
            file=sys.stderr,
        )
        return 2

    if args.no_auto_start:
        print("Argument parsing succeeded.")
        return 0

    inspector = ACPInspector(command=command, session_id=args.session_id)
    try:
        inspector.start()
        return run_repl(inspector)
    except InspectorError as exc:
        print(f"Startup error: {exc}", file=sys.stderr)
        return 1
    finally:
        inspector.stop()


if __name__ == "__main__":
    raise SystemExit(main())
