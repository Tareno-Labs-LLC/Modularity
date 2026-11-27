"""Local Modularity companion for the official Codex Python SDK."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
from threading import Lock
import webbrowser

from openai_codex import ApprovalMode, Codex, CodexConfig, Sandbox
from openai_codex.client import CodexClient
from openai_codex.generated.v2_all import GetAccountRateLimitsResponse


def wire(model):
    return model.model_dump(mode="json", by_alias=True, exclude_none=True)


class Workspace:
    def __init__(self, project, edits, network, cli):
        self.project = project
        self.edits = edits
        self.network = network
        search_mode = "live" if network else "disabled"
        overrides = (
            f'sandbox_workspace_write.network_access={str(edits and network).lower()}',
            f'web_search="{search_mode}"',
        )
        environment = {key: value for key, value in os.environ.items()
                       if key not in {"OPENAI_API_KEY", "CODEX_API_KEY"}}
        self.config = CodexConfig(codex_bin=cli, cwd=str(project), env=environment,
                                  config_overrides=overrides, client_name="modularity_codex_plugin",
                                  client_title="Modularity Codex Plugin")
        self.codex = Codex(self.config)
        self.thread = None
        self.turn = None
        self.login = None
        self.run_lock = Lock()

    def account(self):
        account = wire(self.codex.account()).get("account", {})
        kind = account.get("type", "")
        return {"signedIn": kind == "chatgpt", "plan": account.get("planType", ""),
                "project": self.project.name, "edits": self.edits, "network": self.network}

    def limits(self):
        client = CodexClient(self.config)
        try:
            client.start()
            client.initialize()
            result = client.request("account/rateLimits/read", None, response_model=GetAccountRateLimitsResponse)
            windows = wire(result).get("rateLimits", {})
            return {key: {field: value for field, value in windows[key].items()
                          if field in {"usedPercent", "windowDurationMins", "resetsAt"}}
                    for key in ("primary", "secondary") if windows.get(key)}
        finally:
            client.close()

    def stream(self, prompt, emit):
        if not self.account()["signedIn"]:
            raise ValueError("Sign in with ChatGPT before sending a prompt.")
        if self.thread is None:
            self.thread = self.codex.thread_start(
                cwd=str(self.project), sandbox=Sandbox.workspace_write if self.edits else Sandbox.read_only,
                approval_mode=ApprovalMode.deny_all,
                developer_instructions="You are assisting with a Modularity game project. "
                "Use its saved project files as context. Follow the workspace permissions for this task.",
            )
        self.turn = self.thread.turn(prompt)
        streamed_messages = set()
        try:
            for event in self.turn.stream():
                if event.method == "item/agentMessage/delta":
                    if event.payload.item_id not in streamed_messages:
                        if streamed_messages:
                            emit("text", "\n\n")
                        streamed_messages.add(event.payload.item_id)
                    emit("text", event.payload.delta)
                elif event.method in {"item/started", "item/completed"}:
                    item = wire(event.payload)["item"]
                    kind = item.get("type")
                    if kind in {"commandExecution", "fileChange", "mcpToolCall", "dynamicToolCall", "webSearch"}:
                        emit("tool", {"id": item["id"], "kind": kind, "name": item.get("tool", ""),
                                      "status": item.get("status") or event.method.split("/")[-1]})
                    if (event.method == "item/completed" and kind == "agentMessage"
                            and item["id"] not in streamed_messages and item.get("text")):
                        emit("text", ("\n\n" if streamed_messages else "") + item["text"])
                        streamed_messages.add(item["id"])
                elif event.method == "thread/tokenUsage/updated":
                    emit("usage", wire(event.payload).get("tokenUsage", {}).get("last", {}))
                elif event.method == "turn/completed":
                    result = wire(event.payload)["turn"]
                    if result.get("error"):
                        emit("error", result["error"].get("message", "Codex turn failed."))
                    emit("done", result.get("status", "completed"))
        except (BrokenPipeError, ConnectionResetError):
            self.turn.interrupt()
            raise
        finally:
            self.turn = None


def handler_for(workspace, token):
    page = Path(__file__).with_name("index.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def reply(self, status, payload):
            body = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def trusted(self):
            host = f"127.0.0.1:{self.server.server_port}"
            return (self.headers.get("Host") == host and
                    self.headers.get("Origin", f"http://{host}") == f"http://{host}" and
                    secrets.compare_digest(self.headers.get("X-Modularity-Session", "").encode(), token.encode()))

        def do_GET(self):
            if self.path == "/" and self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}":
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.send_header("Referrer-Policy", "no-referrer")
                self.send_header("Content-Security-Policy", "default-src 'none'; connect-src 'self'; "
                                 "script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(page)
            elif not self.trusted():
                self.reply(403, {"error": "Open the session URL printed by the plugin."})
            elif self.path == "/account":
                self.action(workspace.account)
            elif self.path == "/limits":
                self.action(workspace.limits)
            else:
                self.reply(404, {"error": "Unknown endpoint."})

        def action(self, callback):
            try:
                self.reply(200, callback())
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as error:
                self.reply(502, {"error": str(error)})

        def do_POST(self):
            if not self.trusted():
                self.reply(403, {"error": "Invalid local session."})
                return
            if self.path == "/stop":
                turn = workspace.turn
                self.action(lambda: {"stopped": bool(turn is not None and turn.interrupt() is not None)})
                return
            if self.path not in {"/run", "/login", "/new"}:
                self.reply(404, {"error": "Unknown endpoint."})
                return
            if not workspace.run_lock.acquire(blocking=False):
                self.reply(409, {"error": "A Codex turn is already running."})
                return
            try:
                if self.path == "/new":
                    workspace.thread = None
                    self.reply(200, {})
                elif self.path == "/login":
                    def login():
                        workspace.login = workspace.codex.login_chatgpt()
                        return {"url": workspace.login.auth_url}
                    self.action(login)
                else:
                    self.run_prompt()
            finally:
                workspace.run_lock.release()

        def run_prompt(self):
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 131072:
                    raise ValueError()
                prompt = json.loads(self.rfile.read(length))["prompt"]
                if not isinstance(prompt, str) or not prompt.strip():
                    raise ValueError()
            except (ValueError, KeyError, TypeError):
                self.reply(400, {"error": "Enter a non-empty prompt under 128 KiB."})
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()

            def emit(kind, value):
                self.wfile.write((json.dumps({"kind": kind, "value": value}) + "\n").encode())
                self.wfile.flush()

            try:
                workspace.stream(prompt, emit)
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as error:
                emit("error", str(error))

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path.cwd(), help="Project directory or .modu descriptor")
    parser.add_argument("--allow-edits", action="store_true", help="Allow Codex to edit project files")
    parser.add_argument("--network", action="store_true", help="Enable live search and, with edits enabled, shell networking")
    parser.add_argument("--codex-bin", help="Use a particular compatible Codex executable")
    parser.add_argument("--no-browser", action="store_true", help="Print the local URL only")
    args = parser.parse_args()
    path = args.project.resolve()
    if path.is_dir():
        path = path / "project.modu"
    if not path.is_file() or path.suffix.lower() != ".modu":
        parser.error("Specify a Modularity project directory or its .modu descriptor.")
    workspace = Workspace(path.parent, args.allow_edits, args.network, args.codex_bin)
    try:
        token = secrets.token_urlsafe(32)
        with ThreadingHTTPServer(("127.0.0.1", 0), handler_for(workspace, token)) as server:
            url = f"http://127.0.0.1:{server.server_port}/#{token}"
            print(url, flush=True)
            if not args.no_browser:
                webbrowser.open(url)
            server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        workspace.codex.close()


if __name__ == "__main__":
    main()
