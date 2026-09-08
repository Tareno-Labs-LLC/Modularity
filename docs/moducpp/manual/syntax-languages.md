# Syntax Languages

## Overview
ModuCPP can be written in more than one human language. German is the first officially supported alternative to English.

This is not a second language and not a dialect. A syntax language is a dictionary. The lexer maps the localized spellings back onto the canonical (English) ModuCPP tokens before anything else runs, so the parser, the transpiler, the C++ backend, IntelliSense and the formatter only ever see one language.

```text
Language Dictionary
        |
      Lexer
        |
Canonical Tokens
        |
     Parser
        |
       AST
        |
   C++ Backend
```

There is exactly one parser. There is no `EnglishParser` and no `GermanParser`.

## Why This Feature Exists
Keyword vocabulary is a real barrier for people learning to script, and it is the part of a programming language that translates cleanly. Type names, control flow and declarations are a small closed set, so they can be aliased safely. Everything else in a script is authored by the user and must never be touched.

Localizing the keyword layer lets a team read and write scripts in its own language without forking the toolchain, splitting the ecosystem, or producing behavior that differs from an English project.

## What Gets Translated
Only two categories:

1. Language keywords, such as `public`, `class`, `void`, `if`, `else`, `return`, `add`.
2. Officially localized built-in API aliases, such as `timer.Start` and `timer.Ready`.

Nothing else is translated. The following are never renamed:

- user-created classes
- user-created fields and variables
- methods
- namespaces
- assets
- project identifiers
- string and character literal contents
- comments
- preprocessor lines

## German Example
These two scripts are the same script. They produce byte-identical generated C++.

```moducpp
add ModuCPP;
public class PlayerMovement : ModuNode {
    void TickUpdate() {
        if (true)
            return;
    }
}
```

```moducpp
Nutze ModuCPP;
öffentlich klasse PlayerMovement : ModuNode {
    Nichts Tick() {
        Wenn (true)
            Zurück;
    }
}
```

Working examples live in `Scripts/German Translation ModuCPP/`.

## German Dictionary
### Keywords
| German | ModuCPP |
| --- | --- |
| `Nutze` | `add` |
| `markiere` | `mark` |
| `öffentlich` | `public` |
| `privat` | `private` |
| `geschützt` | `protected` |
| `klasse` | `class` |
| `aufzählung` | `enum` |
| `UnterSkript` | `SubScript` |
| `inspektor` | `inspector` |
| `verweis` | `ref` |
| `zu` | `to` |
| `dann` | `then` |
| `für` | `each` |
| `Wenn` | `if` |
| `sonst` | `else` |
| `solange` | `while` |
| `wiederhole` | `for` |
| `Zurück` | `return` |
| `abbrechen` | `break` |
| `fortfahren` | `continue` |
| `wahr` | `true` |
| `falsch` | `false` |
| `Nichts` | `void` |
| `Zeichenkette` | `string` |
| `Ganzzahl` | `int` |
| `Kommazahl` | `float` |
| `Wahrheitswert` | `bool` |
| `statisch` | `static` |
| `konstant` | `const` |
| `Beginnen` | `Begin` |
| `Start` | `Begin` |
| `Tick` | `TickUpdate` |
| `Aktualisieren` | `Update` |

Where German capitalization is genuinely ambiguous, both spellings are accepted (`Wenn` and `wenn`, `Zurück` and `zurück`, `Nichts` and `nichts`, and so on). When translating a project into German, the first spelling in this table is the one that gets written.

### Built-in API aliases
These only match directly after `.`, `::` or `->`, so a user field called `Zustand` is never touched in any other position.

| German | ModuCPP |
| --- | --- |
| `Starten` | `Start` |
| `Bereit` | `Ready` |
| `Zustand` | `state` |
| `Länge` | `Length` |
| `IstLeer` | `IsEmpty` |

## How It Works
The language pack lives in `src/ModuCPPLanguagePack.h` and `src/ModuCPPLanguagePack.cpp`.

`ModuCPPTranspiler::transpile()` calls `ModuCPPLang::CanonicalizeAuto()` on the source before any other pass runs. Detection is automatic and per file, so the compiler never needs to know what the project preference is. A script written in German compiles in an English project without any configuration.

Two properties matter for tooling:

- Tokens are substituted in place. Nothing is inserted or removed outside a token span, so line numbers in diagnostics still point at the author's source.
- Comments, string and character literals, and preprocessor lines are skipped. `"Wenn"` inside a string stays literal text.

Identifiers may use the script author's alphabet. Bytes above ASCII count as identifier characters throughout the transpiler, so a field named `bodenPrüfDistanz` survives parsing, inspector generation and code generation intact.

## Language Manager
Project Settings has a Language Manager tab. It carries two independent settings:

- Editor Language: the language the editor UI is displayed in. Global, per user. See [Editor Localization](../../EditorLocalization.md).
- ModuCPP Code Language: the localized syntax this project's scripts are written in.

They are never tied together. "German editor, English scripts" and the reverse both work.

Under ModuCPP Code Language:

- Use Global Default: follow the global code language instead of storing a per-project choice.
- Code Language: this project's syntax language.
- Global Default: what new projects start with, and the fallback for projects that do not override it.
- Translate Scripts: rewrites every `.moducpp` file under `Assets/` into the selected language.
- Keyword Dictionary: the exact dictionary the selected language compiles through.
- Import Translation: what to do when a script written in another supported language is imported.

The per-project choice is stored in `project.modu` as `moducppSyntaxLanguage` (blank means "follow the global default"), and the import behavior as `moducppImportTranslatePolicy` (`Ask`, `Always` or `Never`). The global default lives in `launcher_settings.modu` as `moducppDefaultLanguage`.

## Importing Scripts From Another Language
When scripts are imported into a project whose syntax language differs from the imported source, the editor detects the source language and asks whether to translate them into the project's language. The popup has a "Don't ask again" checkbox. Ticking it stores the answer in `project.modu` so future imports follow it automatically. The Language Manager tab can re-enable the prompt.

Declining is always safe. Imported scripts compile and behave identically whether they are translated or not.

## Adding A Language
Add a `Resources/Languages/<Name>/ModuCPP.json` file. No recompile:

```json
{
  "language": { "id": "pirate", "displayName": "Pirate", "endonym": "Pirate" },
  "aliases": [
    { "localized": "hoist",  "canonical": "add" },
    { "localized": "crew",   "canonical": "class" },
    { "localized": "belay",  "canonical": "return" },
    { "localized": "Starten", "canonical": "Start", "position": "member" },
    { "localized": "Start",  "canonical": "Begin", "identifying": false }
  ]
}
```

`position` is `"keyword"` (the default, never matches after `.`, `::` or `->`) or `"member"` (only matches there). `identifying: false` marks spellings that are also plausible English identifiers, so they translate but never vote in automatic detection. Array order matters: when translating *into* a language, the first alias listed for a canonical token is the spelling that gets written.

A file replaces the built-in dictionary for its id, or registers a language the engine has never heard of. The compiled-in English and German tables stay as the fallback when files are missing or malformed, so headless transpiles and broken installs still work.

The built-in table lives in `kLanguageTable` in `src/ModuCPPLanguagePack.cpp`. Either way, nothing else changes:

- no new parser
- no new backend
- no branch anywhere in the transpiler
- no change to the Language Manager UI, which is generated from the registry

Language files are re-read without restarting via Project Settings > Language Manager > Reload Language Files.

## Caveats
- Translation is keyword level. If a user names a field exactly like a keyword in the language the file is detected as, that name is a keyword in that language, the same as in any other programming language.
- The built-in script editor's tokenizer is ASCII only, so localized spellings with characters such as `ö` or `ü` do not receive keyword coloring inside the editor yet. Compilation is unaffected. The VS Code grammar in `moducpp.tmLanguage.json` does highlight them.
- Diagnostics keep correct line numbers across languages. Column numbers can shift because localized spellings differ in length.

## Related Pages
- [Editor Localization](../../EditorLocalization.md)
- [Transpilation Overview](transpilation.md)
- [Script Structure](script-structure.md)
- [Imports and Modules](imports-and-modules.md)
