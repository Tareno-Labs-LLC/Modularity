# ModuCPP Manual

## Overview
The manual explains how to think in ModuCPP before you drop into item-by-item reference pages. It is written for the common scripting questions that come up during real work:

- how should a script be structured?
- which module should be imported for this feature?
- when should logic go in `Begin()` versus `Update()`?
- how should data be exposed to the inspector without turning the script into a pile of public state?
- what makes a script stable enough for gameplay versus editor-only experimentation?

If you are learning the language, the manual is the right starting point. The API reference assumes you already know the problem you are solving. The manual is where the “why” and “when” are explained.

## Why This Section Exists
Most scripting issues are not caused by missing type names. They come from unclear mental models.

A developer might know that `SceneObj` exists, for example, but still not know whether to cache state in fields, look up values every frame, expose references through the inspector, or place setup logic in `Begin()`. Those are workflow questions, not raw reference questions.

The manual exists to answer those workflow questions directly. Each page focuses on one practical area and explains the problem it solves, when you should use it, how it behaves at runtime, and what patterns tend to stay maintainable as scripts grow.

## When To Use The Manual
Use the manual when:

- you are new to ModuCPP
- you are porting an older script to the modular import system
- you know the feature you want, but not the recommended workflow
- you are writing larger gameplay scripts and want examples that look like real production code
- you are debugging behavior and need to understand order of execution or editor/runtime boundaries

Once the concept is clear, move into the API pages for exact members and syntax details.

## Suggested Reading Order
### 1. Start with the basic scripting model
- [Getting Started](getting-started.md)
- [Script Structure](script-structure.md)

These pages explain what a ModuCPP script is, how imports and classes fit together, and how to read the language without mixing it up with plain `C++`.

### 2. Learn the stable runtime workflow
- [Imports and Modules](imports-and-modules.md)
- [Fields and Inspector](fields-and-inspector.md)
- [Methods and Lifecycle](methods-and-lifecycle.md)

These pages cover the features you use in almost every gameplay script.

### 3. Learn the engine-facing helpers
- [Input and Engine Scripting](input-and-engine-scripting.md)
- [Editor Scripting](editor-scripting.md)
- [Common Patterns](common-patterns.md)

These pages show how scripts interact with gameplay objects, input, inspector data, and editor tooling in more complete workflows.

### 4. Read the advanced and maintenance-oriented pages
- [Experimental Features](experimental-features.md)
- [Syntax Languages](syntax-languages.md)
- [Transpilation Overview](transpilation.md)
- [Writing Clean Scripts](writing-clean-scripts.md)
- [Troubleshooting](troubleshooting.md)

These pages help once you are already building larger systems or debugging the edges of the scripting stack.

## Manual Pages
### Core usage
- [Getting Started](getting-started.md): first script, first import, and the current modular model
- [Script Structure](script-structure.md): how a script is laid out and how the transpiler reads it
- [Imports and Modules](imports-and-modules.md): what each module is for and how to choose the right one
- [Fields and Inspector](fields-and-inspector.md): public data, serialized values, and inspector-facing declarations
- [Methods and Lifecycle](methods-and-lifecycle.md): runtime hooks, editor hooks, and update behavior over time

### Engine and tool usage
- [Input and Engine Scripting](input-and-engine-scripting.md): input polling, movement helpers, runtime object control
- [Editor Scripting](editor-scripting.md): editor-only hooks, widgets, tooling workflows, and safety boundaries
- [Experimental Features](experimental-features.md): what is available but should not be treated as stable core surface

### Practical workflows
- [Common Patterns](common-patterns.md): practical script organization and gameplay patterns
- [Syntax Languages](syntax-languages.md): writing ModuCPP in another human language, and the Language Manager
- [Editor Localization](../../EditorLocalization.md): translating the editor UI itself, separately from the code language
- [Transpilation Overview](transpilation.md): high-level explanation of how scripts map to generated `C++`
- [Writing Clean Scripts](writing-clean-scripts.md): maintainability guidance for larger codebases
- [Troubleshooting](troubleshooting.md): common failure modes, mismatched imports, hook issues, and debugging guidance

## How To Read These Pages
Read each page in order the first time. The pages are written to build context gradually. Later, when you already know the basics, they also work as targeted refreshers.

Each page follows the same broad pattern:

- what the feature is
- why it exists
- when to use it
- how it behaves
- realistic examples
- caveats and best practices

That structure is deliberate. It keeps the documentation useful during both onboarding and day-to-day scripting work.

## Related Pages
- [ModuCPP Overview](../README.md)
- [API Reference Index](../api/README.md)
