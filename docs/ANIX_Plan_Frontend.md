# ModuCPP frontend for ANIX v2

ModuCPP can author Abora ANIX Plan v1 files without starting Modularity or
loading the game runtime.

```moducpp
add ANIX;

int main() {
    ANIX::Plan plan;
    plan.Set("hostname", "everest");
    plan.Enable("bluetooth");
    plan.Package("firefox");
    plan.Finish();
    return 0;
}
```

Run it directly:

```bash
tools/moducpp-anix workstation.moducpp
```

Or install the frontend command for ANIX discovery:

```bash
install -Dm755 tools/moducpp-anix ~/.local/bin/moducpp-anix
```

The tool replaces the explicit `add ANIX;` module import, compiles the script
as an ordinary user with the header-only `AnixPlanScriptApi.h`, and executes it
to standard output. It does not load engine libraries, modify ANIX state, or
request elevated permissions. ANIX validates the resulting JSON independently.

The standalone ANIX frontend intentionally uses ModuCPP's C++ pass-through
surface rather than gameplay classes and lifecycle hooks; system configuration
is a one-shot plan, not an engine component.
