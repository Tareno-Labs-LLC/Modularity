#include "ModuUIScriptApi.h"

// Compile the public SDK independently from Engine.h and the editor.
void CheckExpandedUiScriptApi(ScriptContext& context) {
    auto ui = ModuCPP::MakeUIProxy(context, "ui-api-check");
    ui.Toggle.Value = true;
    ui.Toggle.OnChanged([](bool) {});
    ui.Input.Text = "text";
    ui.Input.Placeholder = "hint";
    ui.Input.OnChanged([](const std::string&) {});
    ui.Input.OnSubmit([](const std::string&) {});
    ui.ClearItems(); ui.AddItem("Root"); ui.AddItem("Child", 0);
    ui.SetSelectedIndex(1); ui.GetSelectedItem();
    ui.OnSelectionChanged([](int) {}); ui.OnChanged([]() {});
    ui.SetRange(.1f, .9f); ui.GetRange();
    ui.SetNumericValue(glm::vec4(1)); ui.GetNumericValue();
    ui.SetProgress(.5f); ui.SetLinkedObject(-1); ui.SetFilter("query"); ui.SetOpen(true); ui.Focus();
}
