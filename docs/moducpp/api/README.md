# ModuCPP API Reference

## Overview
The API reference describes the current script-facing surface of ModuCPP in concrete terms. Use it when you already know the concept you are working with and need exact details about a type, module, hook, helper, or script-facing namespace.

Where the manual explains workflows, the API pages explain the actual pieces those workflows are built from.

For example:

- the manual explains when lifecycle hooks should be used
- the API reference explains what each hook does and what it is allowed to do
- the manual explains why the modular import split exists
- the API reference explains what each imported module actually exposes

That split is intentional. It keeps concept pages readable without turning reference pages into shallow lists of names.

## Why This Section Exists
ModuCPP is not a giant open-ended standard library. It is a bounded scripting surface backed by specific headers, runtime facades, and transpiler rules in this repository.

That means the reference pages need to do more than repeat names. They need to answer questions such as:

- which module provides this helper?
- is this safe for gameplay runtime, editor use, or both?
- does this type represent real engine state or only a script-facing façade?
- is this stable core API, example-project API, or experimental surface?

The API reference is where those distinctions are made explicit.

## When To Use The API Reference
Use the API pages when:

- you already know the concept and want exact syntax
- you need to check which module must be imported
- you want to compare similar helpers or script bases
- you are validating whether something is stable, experimental, or example-only
- you are updating older docs or scripts and need current names and behavior

If you are still trying to understand the larger scripting workflow, read the manual first. If you already know the workflow and want precise detail, stay here.

## How The Reference Is Organized
### Module pages
The module pages explain what each `add ...;` import means and what area of responsibility it owns.

- [Module: ModuCPP](module-moducpp.md)
- [Module: ModuEngine](module-moduengine.md)
- [Module: ModuInput](module-moduinput.md)
- [Module: RMeshBuilder](module-rmeshbuilder.md)
- [Module: ModuCPP.Experimental](module-moducpp-experimental.md)

Start here if the main question is “Which import am I supposed to use?”

### Type and area pages
These pages document the specific script-facing building blocks you use after the correct modules are in place.

- [ModuNode and ModuBehaviour](type-modunode-and-modubehaviour.md)
- [SceneObj](type-sceneobj.md)
- [ScriptContext](type-scriptcontext.md)
- [Facades and Helper Types](type-facades-and-helpers.md)
- [Managed Bridge (`ModuCPP.cs`)](managed-bridge.md)
- [Hook Reference](hook-reference.md)
- [Inspector Reference](inspector-reference.md)
- [Standalone Movement API](type-standalone-movement.md)
- [DialoguePort Namespace](namespace-dialogueport.md)
- [Shipped Example Types](type-shipped-example-types.md)

Use these pages when you need deeper detail about behavior, members, runtime rules, and realistic usage.

## Reading Tips
Start with the module page for the import you are using. That prevents a common class of scripting mistakes where a page is read in isolation and the wrong module assumptions carry into the script.

Then read the type or helper page that matches the work you are doing. For example:

- if you are structuring a new gameplay script, start with [ModuNode and ModuBehaviour](type-modunode-and-modubehaviour.md)
- if you are manipulating objects and components, start with [SceneObj](type-sceneobj.md)
- if you are exposing values to the editor, read [Inspector Reference](inspector-reference.md)
- if you are trying to understand script/environment state, read [ScriptContext](type-scriptcontext.md)

## Scope Notes
- Pages in this section document the current script-facing surface that can be traced in the repository.
- If a feature is real but unstable, the relevant page marks it as experimental.
- If a type comes from shipped example scripts rather than the stable core module set, the relevant page says so directly.
- `RMeshBuilder` is documented accurately as a reserved import with no exposed script helpers yet.

## Related Pages
- [ModuCPP Overview](../README.md)
- [Manual Index](../manual/README.md)
