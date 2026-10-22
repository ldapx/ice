# Schematic symbol library

The editor includes a small built-in library of common electrical symbols:

- resistor
- capacitor
- diode
- voltage source
- ground
- operational amplifier
- NMOS transistor

Use **Project → Place Symbol** or press `P` in the editor. Symbols are stored in
the canonical schematic model with stable IDs and explicit pin IDs, so placement,
movement, deletion, and connections survive project save/reload.

The initial vector artwork follows the same compact SVG-oriented approach as
[ElectricalSymbolLibrary](https://github.com/basverdoes/ElectricalSymbolLibrary).
Its upstream artwork remains separately licensed; ICE currently uses its own
minimal inline vector artwork and does not copy the upstream asset bundle.
