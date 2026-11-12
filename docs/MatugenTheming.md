# Optional Matugen editor colors

Open a project, then open **Workflow > Style > Style Editor** and enable **Use Matugen
colors**. The option is off by default. It changes the editor's displayed colors
while retaining the project's font, layout, shading, sizes, and transparency.
Disabling it fades back to the saved project colors. Preset and theme exports
save the underlying colors, including while a palette fade is in progress.

The setting is stored in the existing project theme file:

```ini
# ProjectUserSettings/EditorTheme.modutheme
matugen.enabled=true
matugen.palette=
matugen.fadeSeconds=0.35

# Existing color.* and shade.* settings remain the project's own theme.
```

An empty palette path uses `$XDG_CONFIG_HOME/matugen/colors.css`, falling back to
`~/.config/matugen/colors.css` when XDG_CONFIG_HOME is unset. An explicit local
path can be entered in Style Editor (press Enter to apply); `~/` is supported.
Avoid sharing machine-specific absolute paths with collaborators. Their default
palette path resolves against their own environment.

This reads the same generated GTK CSS palette used by the desktop theme. It
does not invoke Matugen or modify its config, templates, or generated files.
The palette must contain literal hexadecimal declarations for these roles:

```css
@define-color theme_bg #181114;
@define-color theme_panel #21191d;
@define-color theme_text #eedfe3;
@define-color theme_accent #fdb0d5;
```

Additional supported roles are `theme_button`, `theme_muted`, `theme_hover`,
`theme_select`, and `theme_edge`; omitted roles use the required palette colors.
Expressions such as `mix(...)` and CSS imports are ignored; this is a color
palette reader, not a GTK CSS evaluator. Other Matugen output formats need a
matching CSS template or conversion to these roles.

File metadata and reads run in a background task about once a second, only when
enabled. Changes, including atomic replacements, fade from the currently
displayed palette, so another update during a fade does not jump back. The
duration is configurable (zero applies immediately). Missing files fall back to
project colors. Incomplete or invalid writes retain the last valid palette and
are retried; Style Editor displays the load status. Switching projects resets
the overlay. The palette reader and transition state are excluded from
`MODULARITY_RUNTIME_ONLY` builds.
