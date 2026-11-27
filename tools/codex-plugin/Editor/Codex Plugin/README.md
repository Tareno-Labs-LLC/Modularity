# Codex Plugin for Modularity

In your project, open **Packages → ModuPAK Manager** and install **Codex Plugin**. The plugin opens a local browser companion.

To try the refreshed UI from a local `Codex Plugin.modupak`, use **Packages → Import ModuPAK**. The updated editor uses the imported companion when you open **Tools → Codex Plugin** in that project.

On Linux editors with automatic setup, choose **Unpack and Open**, or **Open Codex Plugin** if it is already installed. The editor shows setup progress and installs the SDK into the project's Python environment. The OpenGL editor can render the companion in a dockable panel when built with WPE WebKit 2.54 or newer and run with a matching WPE installation. Choose **Tools → Codex Plugin** to reopen the panel. On Vulkan or when WPE is unavailable, the panel offers **Open in System Browser**. ChatGPT sign-in uses the system browser in either mode. Python 3.10 or newer with `venv` support is required.

## Manual setup

From the project root, install the pinned Codex SDK into a project-local Python environment. Use Python 3.10 or newer.

```bash
plugin="Library/InstalledPackages/moduengine.codex-plugin-1.0.0/Editor/Codex Plugin"
python3 -m venv Library/CodexPlugin/venv
Library/CodexPlugin/venv/bin/python -m pip install -r "$plugin/requirements.txt"
Library/CodexPlugin/venv/bin/python "$plugin/codex_plugin.py"
```

If your project uses the `Packages` directory, set `plugin` to `Packages/moduengine.codex-plugin-1.0.0/Editor/Codex Plugin`. If you installed the standalone archive via **Import ModuPAK**, set it to `Editor/Codex Plugin`.

On Windows, use `py -3` to create the environment and `Library\CodexPlugin\venv\Scripts\python.exe` for the final two commands. In PowerShell, assign the path with `$plugin = "..."`.

Choose **Sign in with ChatGPT** in the companion. Codex manages its own sign-in. Codex applies the signed-in account's plan limits, and the sidebar refreshes the usage windows reported by Codex every 30 seconds and after each turn. A plan or workspace policy can restrict tool availability.

The companion uses saved files from the selected project. Pass `--project /path/to/project.modu` to select a different project. Follow-up prompts share the active thread. **New conversation** starts a fresh thread. The conversation shows streamed responses and activity for commands, file changes, search, and configured Codex tools such as MCP tools.

The default session uses Codex's read-only sandbox with web search and sandboxed shell networking disabled. Launch with `--allow-edits` to allow workspace writes. Launch with `--network` to enable live web search. Together these two flags enable shell networking under the workspace-write sandbox. Existing Codex configuration determines available MCP servers and their access. Use **Stop** to interrupt a turn and Ctrl+C in the terminal to close the companion.

[Codex SDK](https://learn.chatgpt.com/docs/codex-sdk) · [Codex app-server](https://learn.chatgpt.com/docs/app-server)
