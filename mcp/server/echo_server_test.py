"""Black-box tests for the echo server over real subprocess stdio pipes."""

import json
import os
import select
import subprocess
import sys
import time
import unittest


TIMEOUT_SECONDS = 5
MAX_REPLY_BYTES = 256 * 1024
SERVER = None


def request(method, request_id=1, **params):
    params["_meta"] = {
        "io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientInfo": {
            "name": "integration-test",
            "version": "1",
        },
        "io.modelcontextprotocol/clientCapabilities": {},
    }
    return {"jsonrpc": "2.0", "id": request_id, "method": method,
            "params": params}


class StdioProcess:
    """Small independent client with bounded reads and guaranteed cleanup."""

    def __init__(self):
        self.process = subprocess.Popen(
            [SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0,
        )
        self.buffer = bytearray()
        self.finished = False

    def __enter__(self):
        return self

    def close_input(self):
        if self.process.stdin is not None:
            self.process.stdin.close()
            # communicate() must not flush an already closed stream.
            self.process.stdin = None

    def __exit__(self, exc_type, exc_value, traceback):
        if not self.finished:
            if self.process.poll() is None:
                self.process.kill()
            self.close_input()
            self.process.communicate(timeout=TIMEOUT_SECONDS)
        for stream in (self.process.stdin, self.process.stdout,
                       self.process.stderr):
            if stream is not None:
                stream.close()

    def send_raw(self, payload):
        offset = 0
        while offset < len(payload):
            written = self.process.stdin.write(payload[offset:])
            if not written:
                raise AssertionError("Server stdin stopped accepting input")
            offset += written
        self.process.stdin.flush()

    def send(self, value):
        self.send_raw(json.dumps(value, separators=(",", ":")).encode()
                      + b"\n")

    def read_reply(self):
        deadline = time.monotonic() + TIMEOUT_SECONDS
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("Timed out waiting for a flushed reply")
            ready, _, _ = select.select(
                [self.process.stdout], [], [], remaining,
            )
            if not ready:
                raise AssertionError("Timed out waiting for a flushed reply")
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError("Server stdout closed before a reply")
            self.buffer.extend(chunk)
            if len(self.buffer) > MAX_REPLY_BYTES:
                raise AssertionError("Server reply exceeded the test limit")
        line, _, remainder = self.buffer.partition(b"\n")
        self.buffer = remainder
        reply = json.loads(line)
        if not isinstance(reply, dict) or reply.get("jsonrpc") != "2.0":
            raise AssertionError("Stdout is not a JSON-RPC response")
        if "id" not in reply or (("result" in reply) == ("error" in reply)):
            raise AssertionError("Invalid JSON-RPC response shape")
        return reply

    def exchange(self, value):
        self.send(value)
        reply = self.read_reply()
        if reply["id"] != value["id"]:
            raise AssertionError(
                "Reply ID mismatch or unexpected notification reply",
            )
        return reply

    def finish(self):
        self.close_input()
        stdout, stderr = self.process.communicate(timeout=TIMEOUT_SECONDS)
        self.finished = True
        remaining_stdout = bytes(self.buffer) + (stdout or b"")
        return self.process.returncode, remaining_stdout, stderr


class EchoServerTest(unittest.TestCase):
    def assert_clean_shutdown(self, peer):
        code, stdout, stderr = peer.finish()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(stdout, b"", "Unexpected stdout after replies")
        self.assertEqual(
            stderr, b"", "Unexpected startup or protocol diagnostics",
        )

    def test_live_discovery_catalog_and_call(self):
        with StdioProcess() as peer:
            discovery = peer.exchange(request("server/discover"))["result"]
            self.assertEqual(discovery["resultType"], "complete")
            self.assertEqual(discovery["supportedVersions"], ["2026-07-28"])
            self.assertEqual(discovery["capabilities"], {"tools": {}})
            info = discovery["_meta"]["io.modelcontextprotocol/serverInfo"]
            self.assertEqual(info["name"], "slop-echo-server")
            tools = peer.exchange(request("tools/list", 2))["result"]["tools"]
            self.assertEqual(len(tools), 1)
            self.assertEqual(tools[0]["name"], "echo")
            self.assertEqual(tools[0]["inputSchema"]["required"], ["text"])
            self.assertEqual(tools[0]["outputSchema"], tools[0]["inputSchema"])

            notification = request("tools/call", 3, name="echo",
                                   arguments={"text": "must not reply"})
            del notification["id"]
            peer.send(notification)
            text = "hello\nworld\r\n\u2603"
            result = peer.exchange(request("tools/call", "echo-4", name="echo",
                                           arguments={"text": text}))["result"]
            self.assertEqual(result["resultType"], "complete")
            self.assertFalse(result["isError"])
            self.assertEqual(
                result["content"], [{"type": "text", "text": text}],
            )
            self.assertEqual(result["structuredContent"], {"text": text})
            # Replies arrive while stdin is open, proving response flushes.
            self.assert_clean_shutdown(peer)

    def test_errors_are_protocol_messages_and_server_recovers(self):
        with StdioProcess() as peer:
            peer.send_raw(b"{\n")
            malformed = peer.read_reply()
            self.assertIsNone(malformed["id"])
            self.assertEqual(malformed["error"]["code"], -32700)
            old = request("server/discover", 2)
            old["params"]["_meta"] = {
                "io.modelcontextprotocol/protocolVersion": "2025-11-25",
            }
            error = peer.exchange(old)["error"]
            self.assertEqual(error["code"], -32022)
            self.assertEqual(error["data"]["supported"], ["2026-07-28"])
            classic = {
                "jsonrpc": "2.0", "id": 3, "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25", "capabilities": {},
                    "clientInfo": {"name": "old", "version": "1"},
                },
            }
            self.assertEqual(peer.exchange(classic)["error"]["code"], -32602)
            unknown = request("tools/call", 4, name="missing", arguments={})
            self.assertEqual(peer.exchange(unknown)["error"]["code"], -32602)
            bad_shape = request("tools/call", 5, name="echo", arguments=None)
            self.assertEqual(peer.exchange(bad_shape)["error"]["code"], -32602)
            bad_value = request(
                "tools/call", 6, name="echo", arguments={"text": 7},
            )
            self.assertTrue(peer.exchange(bad_value)["result"]["isError"])
            valid = request(
                "tools/call", 7, name="echo", arguments={"text": "recovered"},
            )
            result = peer.exchange(valid)["result"]
            self.assertEqual(
                result["structuredContent"], {"text": "recovered"},
            )
            self.assert_clean_shutdown(peer)

    def run_once(self, payload, *args):
        return subprocess.run([SERVER, *args], input=payload,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=TIMEOUT_SECONDS, check=False)

    def test_quiet_eof_and_usage(self):
        empty = self.run_once(b"")
        self.assertEqual(empty.returncode, 0)
        self.assertEqual(empty.stdout, b"")
        self.assertEqual(empty.stderr, b"")
        help_result = self.run_once(b"", "--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertEqual(help_result.stdout, b"")
        self.assertIn(b"usage:", help_result.stderr)
        invalid = self.run_once(b"", "unexpected")
        self.assertEqual(invalid.returncode, 2)
        self.assertEqual(invalid.stdout, b"")
        self.assertIn(b"usage:", invalid.stderr)

    def test_truncated_input_is_not_executed(self):
        call = request("tools/call", name="echo", arguments={"text": "secret"})
        result = self.run_once(json.dumps(call).encode())
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"before newline", result.stderr)
        self.assertNotIn(b"secret", result.stderr)

    def test_oversized_input_is_not_executed(self):
        result = self.run_once(b"x" * (1024 * 1024 + 1) + b"\n")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"exceeds byte limit", result.stderr)

    def test_closed_stdout_is_reported_on_stderr(self):
        with StdioProcess() as peer:
            peer.process.stdout.close()
            peer.process.stdout = None
            peer.send(request(
                "tools/call", name="echo", arguments={"text": "hello"},
            ))
            code, stdout, stderr = peer.finish()
            self.assertEqual(code, 1)
            self.assertEqual(stdout, b"")
            self.assertIn(b"MCP output", stderr)


if __name__ == "__main__":
    SERVER = sys.argv.pop(1)
    unittest.main()
