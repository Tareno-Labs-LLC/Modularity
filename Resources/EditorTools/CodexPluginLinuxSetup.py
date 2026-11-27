"""Set up and launch the installed Codex Plugin from the Linux editor."""
import fcntl
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys


def status(stage):
    print(f"MODU_CODEX_STATUS {stage}", flush=True)


def launch(project, package, embedded=False):
    imported = project / "Editor" / "Codex Plugin"
    plugin = imported if (imported / "codex_plugin.py").is_file() else package / "Editor" / "Codex Plugin"
    requirements = plugin / "requirements.txt"
    entry = plugin / "codex_plugin.py"
    if not requirements.is_file() or not entry.is_file():
        raise RuntimeError("Reinstall Codex Plugin in ModuPAK Manager.")

    state = project / "Library" / "CodexPlugin"
    state.mkdir(parents=True, exist_ok=True)
    environment = state / "venv"
    python = environment / "bin" / "python"
    installed = state / "requirements.sha256"
    fingerprint = hashlib.sha256(requirements.read_bytes()).hexdigest()

    status("Preparing Python environment")
    with (state / "setup.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not python.is_file():
            subprocess.run([sys.executable, "-m", "venv", str(environment)], check=True)

        status("Checking Codex SDK")
        current = installed.read_text().strip() if installed.is_file() else ""
        if current != fingerprint or subprocess.run(
            [str(python), "-c", "import openai_codex"], check=False,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode:
            status("Installing Codex SDK")
            if subprocess.run([str(python), "-m", "pip", "--version"], check=False,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                subprocess.run([str(python), "-m", "ensurepip", "--upgrade"], check=True)
            subprocess.run([str(python), "-m", "pip", "--disable-pip-version-check",
                            "install", "-r", str(requirements)], check=True)
            subprocess.run([str(python), "-c", "import openai_codex"], check=True)
            installed.write_text(fingerprint + "\n")

    status("Starting Codex Plugin")
    with subprocess.Popen([str(python), str(entry), "--project", str(project), "--no-browser"],
                          cwd=project, stdout=subprocess.PIPE, text=True) as companion:
        try:
            url = companion.stdout.readline().strip()
            if not re.fullmatch(r"http://127\.0\.0\.1:[0-9]{1,5}/#[A-Za-z0-9_-]{32,}", url):
                raise RuntimeError("The Codex companion failed to start.")
            if embedded:
                print(f"MODU_CODEX_URL {url}", flush=True)
            else:
                browser = subprocess.Popen(["xdg-open", url], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                           stderr=subprocess.DEVNULL, start_new_session=True)
                try:
                    if browser.wait(timeout=1):
                        raise RuntimeError("Linux could not open the default browser.")
                except subprocess.TimeoutExpired:
                    pass
                status("Browser opened")
            code = companion.wait()
            if code:
                raise RuntimeError(f"The Codex companion exited with status {code}.")
        finally:
            if companion.poll() is None:
                companion.terminate()


def main():
    try:
        if sys.version_info < (3, 10):
            raise RuntimeError("Install Python 3.10 or newer with venv support.")
        launch(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(),
               len(sys.argv) > 3 and sys.argv[3] == "embedded")
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Codex Plugin setup failed: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
