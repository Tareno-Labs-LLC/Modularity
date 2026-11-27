"""Build the Codex Plugin archive for Modularity's Import ModuPAK dialog."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def main():
    source = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", type=Path,
                        default=source.parents[1] / "build" / "Codex Plugin.modupak")
    output = parser.parse_args().output
    if output.suffix.lower() != ".modupak":
        parser.error("The output filename must end in .modupak.")
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED) as package:
        package.write(source / "Manifest.modu", "Manifest.modu")
        for name in ("README.md", "requirements.txt", "codex_plugin.py", "index.html"):
            relative = Path("Editor") / "Codex Plugin" / name
            package.write(source / relative, relative.as_posix())
    print(output)


if __name__ == "__main__":
    main()
