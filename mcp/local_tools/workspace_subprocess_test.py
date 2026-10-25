#!/usr/bin/env python3
"""Exercise the stdio server's launch workspace and live worktree discovery."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

PROTOCOL_VERSION = "2026-07-28"


def request(method, request_id, params):
    params["_meta"] = {"io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION}
    return json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})


def read_response(process):
    line = process.stdout.readline()
    if not line:
        raise AssertionError(f"server closed stdout; stderr={process.stderr.read()!r}")
    return json.loads(line)


def run_git(directory, *arguments):
    subprocess.run(["git", "-C", str(directory), *arguments], check=True, capture_output=True, text=True)


def main(server_path):
    with tempfile.TemporaryDirectory(prefix="slop-local-tools-") as temporary_directory:
        parent = Path(temporary_directory)
        repository = parent / "repo with spaces"
        repository.mkdir()
        run_git(repository, "init", "--quiet", "-b", "main")
        run_git(repository, "config", "user.name", "workspace-test")
        run_git(repository, "config", "user.email", "workspace-test@example.invalid")
        run_git(repository, "commit", "--allow-empty", "--quiet", "-m", "initial")

        process = subprocess.Popen(
            [server_path],
            cwd=repository,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        try:
            worktree = parent / "feature worktree"
            run_git(repository, "worktree", "add", "--quiet", "-b", "feature", str(worktree))
            if not (worktree / ".git").is_file():
                raise AssertionError("linked worktree must have a .git file")

            process.stdin.write(request("tools/call", 1, {"name": "workspace_list", "arguments": {}}) + "\n")
            process.stdin.flush()
            response = read_response(process)
            if "error" in response:
                raise AssertionError(f"workspace_list failed: {response['error']}")
            listed = response["result"]["structuredContent"]
            if listed["default"] != "launch":
                raise AssertionError(f"unexpected default workspace: {listed!r}")
            workspaces = {entry["id"]: entry for entry in listed["workspaces"]}
            worktree_id = f"worktree:{worktree.resolve()}"
            if workspaces["launch"]["root"] != str(repository.resolve()):
                raise AssertionError(f"wrong launch root: {workspaces['launch']!r}")
            if workspaces[worktree_id]["root"] != str(worktree.resolve()):
                raise AssertionError(f"wrong worktree root: {workspaces[worktree_id]!r}")

            process.stdin.write(
                request(
                    "tools/call",
                    2,
                    {"name": "workspace_info", "arguments": {"workspace": worktree_id}},
                )
                + "\n"
            )
            process.stdin.flush()
            response = read_response(process)
            if "error" in response:
                raise AssertionError(f"workspace_info failed: {response['error']}")
            selected = response["result"]["structuredContent"]
            if selected["root"] != str(worktree.resolve()):
                raise AssertionError(f"wrong selected root: {selected!r}")

            process.stdin.close()
            if process.wait(timeout=10) != 0:
                raise AssertionError(f"server exited nonzero: {process.stderr.read()!r}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: workspace_subprocess_test.py SERVER")
    main(sys.argv[1])
