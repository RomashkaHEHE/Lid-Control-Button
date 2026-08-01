// ==WindhawkMod==
// @id              settings-controls-test
// @name            Settings Controls Test
// @description     A harmless showcase of all Windhawk mod settings controls
// @version         1.0.0
// @author          Roma
// @include         explorer.exe
// @architecture    x86-64
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Settings Controls Test

A harmless test mod for exploring the settings interface. It doesn't hook
functions or change Windows behavior. Edit the controls, add and remove list
items, and save the settings to see how Windhawk renders each supported type.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- toggle: true
  $name: Toggle
  $description: A boolean value is rendered as a switch.

- number: 15
  $name: Number
  $description: An integer value is rendered as a numeric input.

- text: "Sample text"
  $name: Text
  $description: A string value is rendered as a single-line text box.

- dropdown: "second"
  $name: Dropdown
  $description: A string with $options is rendered as a searchable dropdown.
  $options:
  - first: First option
  - second: Second option
  - third: Third option

- Group:
  - nestedToggle: false
    $name: Nested toggle
  - nestedNumber: 42
    $name: Nested number
  - nestedText: "Inside a group"
    $name: Nested text
  - nestedDropdown: "automatic"
    $name: Nested dropdown
    $options:
    - automatic: Automatic
    - enabled: Enabled
    - disabled: Disabled
  $name: Group
  $description: Nested settings are rendered together in a card.

- numberList: [10]
  $name: Number list
  $description: A repeatable list of numeric inputs with Add and Remove controls.

- textList: [""]
  $name: Text list
  $description: A repeatable list of text boxes with Add and Remove controls.

- dropdownList: ["medium"]
  $name: Dropdown list
  $description: A repeatable list in which every item is a dropdown.
  $options:
  - low: Low
  - medium: Medium
  - high: High

- objectList:
  - - enabled: true
      $name: Enabled
    - label: "Example rule"
      $name: Label
    - count: 3
      $name: Count
    - mode: "include"
      $name: Mode
      $options:
      - include: Include
      - exclude: Exclude
  $name: Object list
  $description: A repeatable list of compound objects containing mixed controls.
*/
// ==/WindhawkModSettings==

BOOL Wh_ModInit() {
    Wh_Log(L"Settings Controls Test loaded");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Settings Controls Test unloaded");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings Controls Test settings saved");
}
