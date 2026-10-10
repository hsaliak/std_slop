import atexit
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class McpHttpHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = request.get("method")
        if method == "server/discover":
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "error": {"code": -32601, "message": "Method not found"},
            }
        elif method == "initialize":
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "gateway-test", "version": "1.0"},
                },
            }
        elif method == "notifications/initialized":
            self.send_response(202)
            self.end_headers()
            return
        elif method == "tools/list":
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {
                    "tools": [
                        {
                            "name": "lookup",
                            "description": "Return the supplied query.",
                            "inputSchema": {
                                "$schema": "http://json-schema.org/draft-07/schema#",
                                "type": "object",
                                "properties": {"query": {"type": "string"}},
                                "required": ["query"],
                            },
                        }
                    ]
                },
            }
        elif method == "tools/call":
            query = request["params"]["arguments"]["query"]
            if query == "slow":
                time.sleep(0.5)
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {"content": [{"type": "text", "text": "http:" + query}], "isError": False},
            }
        else:
            response = {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "error": {"code": -32601, "message": "Method not found"},
            }
        body = json.dumps(response).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, _format, *_args):
        pass


def start_http_mcp_server():
    server = ThreadingHTTPServer(("127.0.0.1", 0), McpHttpHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    atexit.register(server.shutdown)
    return server


def main():
    gateway, echo_server = sys.argv[1:]
    http_server = start_http_mcp_server()
    with tempfile.TemporaryDirectory() as temporary_directory:
        temp = pathlib.Path(temporary_directory)
        config_path = temp / "gateway.json"
        trace_path = temp / "run_js_trace.log"
        config_path.write_text(
            json.dumps(
                {
                    "runTimeoutMs": 30000,
                    "traceLogPath": str(trace_path),
                    "servers": [
                        {
                            "alias": "echo",
                            "transport": "stdio",
                            "command": os.path.abspath(echo_server),
                            "args": [],
                            "allowTools": ["echo"],
                        },
                        {
                            "alias": "echo2",
                            "transport": "stdio",
                            "command": os.path.abspath(echo_server),
                            "args": [],
                            "allowTools": ["echo"],
                        },
                        {
                            "alias": "docs",
                            "transport": "http",
                            "endpointUrl": "http://127.0.0.1:%d/mcp" % http_server.server_address[1],
                            "allowTools": ["lookup"],
                        },
                    ]
                }
            ),
            encoding="utf-8",
        )
        code = (
            "const results = await Promise.all(["
            "echo.echo({text: input.text}), echo2.echo({text: input.other}), "
            "docs.lookup({query: input.query})]); "
            "return results.map(value => value.structuredContent || value.content);"
        )
        requests = [
            {
                "jsonrpc": "2.0",
                "id": "discover",
                "method": "server/discover",
                "params": {"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}},
            },
            {
                "jsonrpc": "2.0",
                "id": "list",
                "method": "tools/list",
                "params": {"_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}},
            },
            {
                "jsonrpc": "2.0",
                "id": "call",
                "method": "tools/call",
                "params": {
                    "name": "run_js",
                    "arguments": {
                        "code": code,
                        "input": {"text": "gateway-smoke", "other": "ipc-parallel", "query": "http-smoke"},
                    },
                    "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
                },
            },
        ]
        process = subprocess.run(
            [gateway, "--config", str(config_path)],
            input="".join(json.dumps(request) + "\n" for request in requests),
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )
        if process.returncode != 0:
            raise AssertionError(f"gateway exited {process.returncode}: {process.stderr}")
        lines = process.stdout.splitlines()
        if len(lines) != 3:
            raise AssertionError(
                f"expected three MCP responses; got stdout={process.stdout!r}, stderr={process.stderr!r}"
            )
        responses = [json.loads(line) for line in lines]
        if responses[0].get("id") != "discover" or "error" in responses[0]:
            raise AssertionError(f"server discovery failed: {responses[0]}")
        if responses[1].get("id") != "list" or "error" in responses[1]:
            raise AssertionError(f"tool listing failed: {responses[1]}")
        names = {tool["name"] for tool in responses[1]["result"]["tools"]}
        if not {"run_js", "run_js_help"}.issubset(names):
            raise AssertionError(f"gateway tools missing: {names}")
        call = responses[2]
        if call.get("id") != "call" or "error" in call:
            raise AssertionError(f"run_js MCP call failed: {call}")
        result = call["result"]["structuredContent"]["result"]
        if result != [{"text": "gateway-smoke"}, {"text": "ipc-parallel"}, [{"type": "text", "text": "http:http-smoke"}]]:
            raise AssertionError(f"unexpected composed result: {result}")

        trace = trace_path.read_text(encoding="utf-8")
        for expected in (
            "event=RUN START",
            "code (terminal-safe; control bytes escaped):\n" + code,
            '"text": "gateway-smoke"',
            '"text": "ipc-parallel"',
            '"query": "http-smoke"',
            "event=TOOL CALL 1",
            "event=TOOL CALL 2",
            "event=TOOL CALL 3",
            "raw downstream result:",
            "JavaScript-visible result:",
            "event=RUN SUCCESS",
        ):
            if expected not in trace:
                raise AssertionError(f"trace log missing {expected!r}: {trace}")
        if trace_path.stat().st_mode & 0o777 != 0o600:
            raise AssertionError(f"trace log permissions are not 0600: {trace_path.stat().st_mode:o}")

        config_path.write_text(
            json.dumps(
                {
                    "runTimeoutMs": 100,
                    "traceLogPath": str(trace_path),
                    "servers": json.loads(config_path.read_text())["servers"],
                }
            ),
            encoding="utf-8",
        )
        timeout_request = {
            "jsonrpc": "2.0",
            "id": "timeout",
            "method": "tools/call",
            "params": {
                "name": "run_js",
                "arguments": {"code": "while (true) {}"},
                "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
            },
        }
        timed = subprocess.run(
            [gateway, "--config", str(config_path)],
            input=json.dumps(timeout_request) + "\n",
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
        if timed.returncode != 0:
            raise AssertionError(f"timed gateway exited {timed.returncode}: {timed.stderr}")
        timed_response = json.loads(timed.stdout)
        error = timed_response["result"]["structuredContent"]["error"]
        if timed_response.get("id") != "timeout" or error["category"] != "budget":
            raise AssertionError(
                f"worker did not report its run deadline: {timed_response}"
            )
        trace = trace_path.read_text(encoding="utf-8")
        if "event=RUN FAILURE" not in trace or "while (true) {}" not in trace:
            raise AssertionError(f"trace log missed the timed-out run: {trace}")

        slow_request = {
            "jsonrpc": "2.0",
            "id": "slow",
            "method": "tools/call",
            "params": {
                "name": "run_js",
                "arguments": {"code": 'await docs.lookup({query: "slow"}); return "late";'},
                "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28"},
            },
        }
        slow = subprocess.run(
            [gateway, "--config", str(config_path)],
            input=json.dumps(slow_request) + "\n",
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
        if slow.returncode != 0:
            raise AssertionError(f"slow downstream gateway exited {slow.returncode}: {slow.stderr}")
        slow_response = json.loads(slow.stdout)
        slow_error = slow_response["result"]["structuredContent"]["error"]
        if slow_response.get("id") != "slow" or slow_error["category"] != "budget":
            raise AssertionError(f"slow HTTP request did not honor run deadline: {slow_response}")

        marker = temp / "should-not-start"
        child = temp / "marker-child.sh"
        child.write_text(f"#!/bin/sh\ntouch '{marker}'\n", encoding="utf-8")
        child.chmod(0o700)
        invalid_config = temp / "invalid.json"
        invalid_config.write_text(
            json.dumps(
                {
                    "servers": [
                        {
                            "alias": "echo",
                            "transport": "http",
                            "endpointUrl": "http://127.0.0.1:1/mcp",
                            "command": str(child),
                            "args": [],
                            "allowTools": ["echo"],
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        invalid = subprocess.run(
            [gateway, "--config", str(invalid_config)],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
        if invalid.returncode == 0 or marker.exists():
            raise AssertionError("invalid configuration started a child or returned success")


if __name__ == "__main__":
    main()
