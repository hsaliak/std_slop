import json
import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    gateway, echo_server = sys.argv[1:]
    with tempfile.TemporaryDirectory() as temporary_directory:
        temp = pathlib.Path(temporary_directory)
        config_path = temp / "gateway.json"
        config_path.write_text(
            json.dumps(
                {
                    "runTimeoutMs": 30000,
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
                    ]
                }
            ),
            encoding="utf-8",
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
                        "code": (
                            "const results = await Promise.all(["
                            "echo.echo({text: input.text}), echo2.echo({text: input.other})]); "
                            "return results.map(value => value.structuredContent);"
                        ),
                        "input": {"text": "gateway-smoke", "other": "ipc-parallel"},
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
        if result != [{"text": "gateway-smoke"}, {"text": "ipc-parallel"}]:
            raise AssertionError(f"unexpected composed result: {result}")

        config_path.write_text(
            json.dumps(
                {"runTimeoutMs": 100, "servers": json.loads(config_path.read_text())["servers"]}
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
