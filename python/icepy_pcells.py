"""Package PCell runtime for :mod:`icepy`.

ICE-native package PCells live in ``<package>/layout/devices/pcell/`` and
build geometry through the normal ``icepy`` layout builder. This module is
technology-neutral: PDK-specific runtimes, such as the IHP PyCell bridge, live
inside their own package.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Package-local PCell runtime
# ---------------------------------------------------------------------------
#
# PDK packages ship ``layout/devices/pcell/pcells.py`` (or another module in
# ``layout/devices/pcell/``) whose classes build geometry through the ``icepy``
# layout builder.
# The generator contract is intentionally small so packages stay independent
# from KLayout, gdsfactory, or any other external PCell runtime.


PACKAGE_DBU_PER_UM = 1000


def _length_um(value, default: float = 0.0) -> float:
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip()
    if not text:
        return default
    # Layout parameters commonly use SPICE-style suffixes (1u, 150n, 2m).
    # Accept explicit physical units as well so hand-written package parameters
    # remain readable.
    units = {
        "meg": 1e3,
        "mil": 25.4,
        "nm": 1e-3,
        "um": 1.0,
        "µm": 1.0,
        "mm": 1e3,
        "m": 1e-3,
        "u": 1.0,
        "n": 1e-3,
        "p": 1e-6,
        "f": 1e-9,
        "k": 1e3,
    }
    for suffix, scale in units.items():
        if text.endswith(suffix):
            try:
                return float(text[: -len(suffix)]) * scale
            except ValueError:
                break
    try:
        return float(text)
    except ValueError:
        return default


def package_parameter_values(specs, overrides: dict[str, str] | None = None):
    values = {}
    overrides = overrides or {}
    for spec in specs:
        name = spec["name"]
        values[name] = overrides.get(name, spec.get("value", ""))
    for name, value in overrides.items():
        values.setdefault(name, value)
    return values


def load_package_pcell_module(pdk_root: str | Path, module_name: str):
    """Import a package PCell module by name from ``<pdk>/layout/devices/pcell``."""
    import importlib.util

    root = Path(pdk_root).resolve()
    candidates = [
        root / "layout" / "devices" / "pcell" / f"{module_name}.py",
        root / "layout" / "devices" / "pcell" / "pcells.py",
    ]
    path = next((candidate for candidate in candidates if candidate.is_file()), None)
    if path is None:
        raise RuntimeError(
            f"PCell module '{module_name}' was not found under "
            f"{root / 'layout' / 'devices' / 'pcell'}"
        )
    unique_name = f"ice_package_pcells_{abs(hash(path))}_{module_name}"
    spec = importlib.util.spec_from_file_location(unique_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load PCell module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def package_pcell_class(pdk_root: str | Path, module_name: str, class_name: str):
    module = load_package_pcell_module(pdk_root, module_name)
    implementation = getattr(module, class_name, None)
    if implementation is None:
        raise RuntimeError(
            f"PCell class '{class_name}' was not found in module '{module_name}'"
        )
    return implementation


def package_parameter_specs(
    pdk_root: str | Path, module_name: str, class_name: str
) -> list[dict[str, str]]:
    implementation = package_pcell_class(pdk_root, module_name, class_name)
    specs = getattr(implementation, "parameters", None)
    if callable(specs):
        specs = specs()
    return [dict(spec) for spec in (specs or [])]


def port_direction_text(direction) -> str:
    return str(direction).split(".")[-1]


def port_side_text(side) -> str:
    return str(side).split(".")[-1]


def generate_package_geometry(
    pdk_root: str | Path,
    module_name: str,
    class_name: str,
    parameters: dict[str, str] | None = None,
) -> dict:
    """Build one package PCell through ``icepy`` and return geometry JSON."""
    import icepy

    implementation = package_pcell_class(pdk_root, module_name, class_name)
    specs = getattr(implementation, "parameters", None)
    if callable(specs):
        specs = specs()
    specs = [dict(spec) for spec in (specs or [])]
    values = package_parameter_values(specs, parameters)

    project = icepy.new_project(f"{class_name}-pcell")
    cell_name = str(getattr(implementation, "name", class_name) or class_name)
    builder = project.create_layout_cell(
        cell_name, str(getattr(implementation, "pdk", ""))
    )
    pcell = implementation(values)
    pcell.build(builder)
    cell = builder.cell()
    layers_by_id = {layer.id: layer for layer in project.layers}
    shapes = []
    for shape in cell.shapes:
        layer = layers_by_id.get(shape.layer)
        if layer is None:
            continue
        points = [[point.x, point.y] for point in shape.polygon]
        if len(points) < 3:
            rectangle = getattr(shape, "rectangle", None)
            if rectangle is not None:
                points = [
                    [rectangle.x, rectangle.y],
                    [rectangle.x + rectangle.width, rectangle.y],
                    [rectangle.x + rectangle.width, rectangle.y + rectangle.height],
                    [rectangle.x, rectangle.y + rectangle.height],
                ]
        if len(points) < 3:
            continue
        shapes.append(
            {
                "layer": int(layer.number),
                "datatype": int(layer.datatype),
                "points": points,
            }
        )
    if not shapes:
        raise RuntimeError(f"PCell '{class_name}' produced no geometry")
    ports = []
    for port in cell.ports:
        layer = layers_by_id.get(port.layer)
        if layer is None:
            continue
        ports.append(
            {
                "name": port.name,
                "layer": int(layer.number),
                "datatype": int(layer.datatype),
                "rectangle": {
                    "x": port.rectangle.x,
                    "y": port.rectangle.y,
                    "width": port.rectangle.width,
                    "height": port.rectangle.height,
                },
                "direction": port_direction_text(port.direction),
                "side": port_side_text(port.side),
                "label": port.label or port.name,
            }
        )
    bounds = cell.bounds
    if bounds is None:
        raise RuntimeError(f"PCell '{class_name}' produced an empty bounding box")
    return {
        "bounds": [
            bounds.x,
            bounds.y,
            bounds.x + bounds.width,
            bounds.y + bounds.height,
        ],
        "shapes": shapes,
        "ports": ports,
    }


def _um_value(value, default: float = 0.0) -> int:
    return int(round(_length_um(value, default) * PACKAGE_DBU_PER_UM))


def _layer_stream(spec) -> tuple[str, int, int]:
    if len(spec) == 2:
        label, number = spec
        return str(label), int(number), 0
    label, number, datatype = spec
    return str(label), int(number), int(datatype)


class PackagePcell:
    """Base class for package PCells built with the ICE layout builder.

    Packages supply a logical layer map and a parameter list. The geometry
    templates below are deliberately technology-neutral: they only use the
    layers the package declares, so the same device model works across PDKs.
    """

    name = ""
    category = ""
    pdk = ""
    family = "mos"
    layers: dict[str, tuple[str, int]] = {}
    parameters: list[dict[str, str]] = []
    pmos = False
    body_layer = "poly"
    doping = "n"
    stack_layers: tuple[str, ...] = ("met1", "met2", "met3")
    cap_bottom = "met1"
    cap_top = "met2"
    cap_marker = ""
    res_layer = "res"
    turns = 2
    guard_ring = False
    # Contact/via cut dimensions and enclosure. Packages override these when
    # their contact rules differ (GF180MCU uses 0.22 um contacts with 0.25 um
    # spacing, SKY130 uses 0.17 um contacts with 0.19 um spacing).
    contact_size = 170
    contact_spacing = 200
    contact_margin = 100

    def __init__(self, values: dict[str, str] | None = None):
        self.values = package_parameter_values(self.parameters, values or {})

    def text(self, name: str, default: str = "") -> str:
        value = self.values.get(name, default)
        return "" if value is None else str(value)

    def number(self, name: str, default: float = 0.0) -> float:
        try:
            return _length_um(self.values.get(name, default), default)
        except (TypeError, ValueError):
            return default

    def um(self, name: str, default: float = 0.0) -> int:
        return int(round(self.number(name, default) * PACKAGE_DBU_PER_UM))

    def integer(self, name: str, default: int = 1) -> int:
        try:
            return int(float(self.values.get(name, default)))
        except (TypeError, ValueError):
            return default

    def boolean(self, name: str, default: bool = False) -> bool:
        value = self.values.get(name)
        if value is None:
            return default
        if isinstance(value, bool):
            return value
        return str(value).strip().lower() in {
            "1",
            "true",
            "yes",
            "on",
            "selected",
            "all",
        }

    def rect(self, builder, layer: str, x, y, width, height):
        if width <= 0 or height <= 0 or layer not in self.layers:
            return
        label, number, datatype = _layer_stream(self.layers[layer])
        builder.layer(label, number, datatype)
        builder.rectangle(
            label, int(round(x)), int(round(y)), int(round(width)), int(round(height))
        )

    def polygon(self, builder, layer: str, points):
        if len(points) < 3 or layer not in self.layers:
            return
        import icepy

        label, number, datatype = _layer_stream(self.layers[layer])
        builder.layer(label, number, datatype)
        builder.polygon(
            label, [icepy.LayoutPoint(int(round(x)), int(round(y))) for x, y in points]
        )

    def port(
        self,
        builder,
        name: str,
        layer: str,
        x,
        y,
        width,
        height,
        direction: str = "inout",
        side: str = "left",
        label: str | None = None,
    ):
        if layer not in self.layers or width <= 0 or height <= 0:
            return
        label_name, number, datatype = _layer_stream(self.layers[layer])
        builder.layer(label_name, number, datatype)
        builder.port(
            name,
            label_name,
            int(round(x)),
            int(round(y)),
            int(round(width)),
            int(round(height)),
            direction=direction,
            side=side,
            label=label or name,
        )

    def build(self, builder):
        method = getattr(self, f"build_{self.family}", None)
        if method is None:
            raise RuntimeError(f"Unsupported PCell family: {self.family}")
        method(builder)

    # -- shared device templates -----------------------------------------
    def _contact_layers(self):
        """Return the contact, landing, and via layer names for this package.

        SKY130 uses licon1 (diff/poly -> li1) and mcon (li1 -> met1).
        GF180MCU uses a single contact layer (diff/poly -> metal1).
        """
        contact = (
            "licon1"
            if "licon1" in self.layers
            else ("contact" if "contact" in self.layers else "")
        )
        landing = "li1" if "li1" in self.layers else ""
        via = "mcon" if "mcon" in self.layers else ""
        return contact, landing, via

    def cut_array(
        self,
        builder,
        layer: str,
        x,
        y,
        width,
        height,
        size=None,
        spacing=None,
        margin=None,
    ):
        """Fill a landing pad with a centered rectangular array of cuts."""
        if not layer or layer not in self.layers:
            return
        size = int(self.contact_size if size is None else size)
        spacing = int(self.contact_spacing if spacing is None else spacing)
        margin = int(self.contact_margin if margin is None else margin)
        size = max(size, 1)
        spacing = max(spacing, 1)
        margin = max(margin, 0)
        usable_w = int(width) - 2 * margin
        usable_h = int(height) - 2 * margin
        if usable_w < size or usable_h < size:
            return
        columns = max(1, (usable_w + spacing) // (size + spacing))
        rows = max(1, (usable_h + spacing) // (size + spacing))
        total_w = columns * size + (columns - 1) * spacing
        total_h = rows * size + (rows - 1) * spacing
        start_x = int(x) + (int(width) - total_w) // 2
        start_y = int(y) + (int(height) - total_h) // 2
        for row in range(rows):
            for column in range(columns):
                self.rect(
                    builder,
                    layer,
                    start_x + column * (size + spacing),
                    start_y + row * (size + spacing),
                    size,
                    size,
                )

    def contact_stack(self, builder, x, y, width, height, cut_size=None):
        """Draw the contact/landing/via stack for a pad on diff or poly."""
        contact, landing, via = self._contact_layers()
        if not contact:
            return
        if landing:
            self.rect(builder, landing, x, y, width, height)
            self.cut_array(builder, contact, x, y, width, height, cut_size)
            if via:
                self.cut_array(builder, via, x, y, width, height, cut_size)
        else:
            self.cut_array(builder, contact, x, y, width, height, cut_size)

    def build_mos(self, builder):
        """Build a non-overlapping multi-finger MOS device.

        Every finger gets an isolated diff island. A single poly gate column
        crosses all fingers and doubles as the gate bus, so fingers are in
        parallel without overlapping diffusion. Source and drain use separate
        metal buses on either side; the body tap sits beyond the drain bus.
        """
        total_width = max(self.um("w", 1.0), 370)
        length = max(self.um("l", 0.15), 50)
        fingers = max(1, self.integer("nf", 1))
        multiplier = max(1, self.integer("m", 1))
        count = fingers * multiplier
        finger_width = max(total_width // fingers, 370)

        enclosure = max(self.contact_margin + 100, 250)
        finger_gap = 2 * enclosure + 300
        pitch = finger_width + finger_gap
        device_height = count * pitch - finger_gap
        source_drain = max(2 * length, 3 * self.contact_size + 2 * self.contact_margin)
        pad_margin = self.contact_margin
        pad_width = max(min(source_drain - 2 * pad_margin, 500), self.contact_size)
        pad_height = max(finger_width - 2 * pad_margin, self.contact_size)
        cut_size = min(
            self.contact_size,
            pad_width - 2 * pad_margin,
            pad_height - 2 * pad_margin,
        )
        cut_size = max(cut_size, 100)

        bus_width = max(min(finger_width // 2, 500), 300)
        bus_gap = 300
        gate_extension = 700
        gate_pad = max(min(length, 400), 370)
        tap_width = max(bus_width, self.contact_size + 4 * self.contact_margin)
        tap_gap = 400
        implant_enclosure = max(self.contact_margin + 100, 250)
        well_enclosure = 300

        pmos = (
            self.pmos
            or "pfet" in self.text("model").lower()
            or "pmos" in self.text("model").lower()
        )
        diff_implant = "psdm" if pmos else "nsdm"
        tap_implant = "nsdm" if pmos else "psdm"
        if diff_implant not in self.layers:
            diff_implant = "pplus" if pmos else "nplus"
        if tap_implant not in self.layers:
            tap_implant = "nplus" if pmos else "pplus"
        tap_layer = "tap" if "tap" in self.layers else "diff"

        # Local geometry anchors.
        source_bus_x = -source_drain - bus_gap - bus_width
        drain_bus_x = length + source_drain + bus_gap
        drain_bus_right = drain_bus_x + bus_width
        tap_x = drain_bus_right + tap_gap
        tap_right = tap_x + tap_width
        source_pad_left = -source_drain + pad_margin
        drain_pad_left = length + source_drain - pad_margin - pad_width
        poly_bottom = -enclosure - gate_extension
        poly_top = (count - 1) * pitch + finger_width + enclosure

        # Well and implant regions are drawn first so the device layers sit on
        # top of them.
        well = "nwell" if pmos else "pwell"
        if well in self.layers:
            self.rect(
                builder,
                well,
                -source_drain - well_enclosure,
                -enclosure - well_enclosure,
                tap_right + source_drain + 2 * well_enclosure,
                device_height + 2 * enclosure + 2 * well_enclosure,
            )
        if diff_implant in self.layers:
            self.rect(
                builder,
                diff_implant,
                -source_drain - implant_enclosure,
                -implant_enclosure,
                length + 2 * source_drain + 2 * implant_enclosure,
                device_height + 2 * implant_enclosure,
            )

        # Each finger is an independent diff island.
        for finger in range(count):
            base = finger * pitch
            self.rect(
                builder,
                "diff",
                -source_drain,
                base,
                length + 2 * source_drain,
                finger_width,
            )
            # Source landing.
            self.contact_stack(
                builder,
                source_pad_left,
                base + pad_margin,
                pad_width,
                pad_height,
                cut_size,
            )
            self.rect(
                builder,
                "met1",
                source_bus_x,
                base + pad_margin,
                source_pad_left + pad_width - source_bus_x,
                pad_height,
            )
            # Drain landing.
            self.contact_stack(
                builder,
                drain_pad_left,
                base + pad_margin,
                pad_width,
                pad_height,
                cut_size,
            )
            self.rect(
                builder,
                "met1",
                drain_pad_left,
                base + pad_margin,
                drain_bus_right - drain_pad_left,
                pad_height,
            )

        # One poly column crosses every finger and acts as the gate bus.
        self.rect(builder, "poly", 0, poly_bottom, length, poly_top - poly_bottom)
        gate_pad_x = length // 2 - gate_pad // 2
        gate_pad_y = poly_bottom + pad_margin
        self.rect(builder, "poly", gate_pad_x, gate_pad_y, gate_pad, gate_pad)
        self.contact_stack(
            builder, gate_pad_x, gate_pad_y, gate_pad, gate_pad, cut_size
        )
        self.rect(builder, "met1", gate_pad_x, gate_pad_y, gate_pad, gate_pad)
        if "npc" in self.layers:
            npc_margin = 100
            self.rect(
                builder,
                "npc",
                gate_pad_x - npc_margin,
                gate_pad_y - npc_margin,
                gate_pad + 2 * npc_margin,
                gate_pad + 2 * npc_margin,
            )

        # Source and drain buses. They run through the gaps between diff
        # islands and never cross a gate.
        bus_bottom = pad_margin - bus_width
        bus_height = (
            (count - 1) * pitch + pad_margin + pad_height + bus_width - bus_bottom
        )
        self.rect(builder, "met1", source_bus_x, bus_bottom, bus_width, bus_height)
        self.rect(builder, "met1", drain_bus_x, bus_bottom, bus_width, bus_height)

        # Body tap and its implant.
        self.rect(
            builder,
            tap_layer,
            tap_x,
            -enclosure,
            tap_width,
            device_height + 2 * enclosure,
        )
        if tap_implant in self.layers:
            self.rect(
                builder,
                tap_implant,
                tap_x - implant_enclosure,
                -enclosure - implant_enclosure,
                tap_width + 2 * implant_enclosure,
                device_height + 2 * enclosure + 2 * implant_enclosure,
            )
        self.contact_stack(
            builder,
            tap_x + pad_margin,
            -enclosure + pad_margin,
            tap_width - 2 * pad_margin,
            device_height + 2 * enclosure - 2 * pad_margin,
            cut_size,
        )
        self.rect(
            builder,
            "met1",
            tap_x + pad_margin,
            -enclosure + pad_margin,
            tap_width - 2 * pad_margin,
            device_height + 2 * enclosure - 2 * pad_margin,
        )

        if (
            self.guard_ring
            or self.boolean("guard_ring")
            or self.text("bulk").lower() in {"guard ring", "gaurd ring"}
        ):
            self.build_guard_ring(
                builder,
                source_bus_x - bus_gap,
                -enclosure - gate_extension - bus_gap,
                tap_right - source_bus_x + 2 * bus_gap,
                poly_top + enclosure + gate_extension + 2 * bus_gap,
                max(bus_width, 300),
            )

        port_height = max(min(device_height, 2 * bus_width), 200)
        self.port(
            builder,
            "G",
            "met1",
            gate_pad_x,
            gate_pad_y,
            gate_pad,
            gate_pad,
            "input",
            "bottom",
        )
        self.port(
            builder,
            "S",
            "met1",
            source_bus_x,
            bus_bottom + (bus_height - port_height) // 2,
            bus_width,
            port_height,
            "inout",
            "left",
        )
        self.port(
            builder,
            "D",
            "met1",
            drain_bus_x,
            bus_bottom + (bus_height - port_height) // 2,
            bus_width,
            port_height,
            "inout",
            "right",
        )
        self.port(
            builder,
            "B",
            "met1",
            tap_x + pad_margin,
            -enclosure + pad_margin,
            tap_width - 2 * pad_margin,
            port_height,
            "inout",
            "right",
        )

    def build_resistor(self, builder):
        length = max(self.um("l", 2.0), 200)
        width = max(self.um("w", 0.5), 100)
        body = self.body_layer
        head = max(length // 4, 200)
        contact = max(min(width // 2, 200), 100)
        self.rect(builder, body, 0, 0, length, width)
        self.rect(builder, "li1", -head, 0, head, width)
        self.rect(builder, "li1", length, 0, head, width)
        self.rect(builder, "met1", -head, -100, head + 100, width + 200)
        self.rect(builder, "met1", length - 100, -100, head + 100, width + 200)
        self.rect(
            builder, "li1", -head + 100, width // 2 - contact // 2, contact, contact
        )
        self.rect(
            builder,
            "li1",
            length + head - 100 - contact,
            width // 2 - contact // 2,
            contact,
            contact,
        )
        if self.res_layer in self.layers:
            self.rect(builder, self.res_layer, -200, -200, length + 400, width + 400)
        if self.doping == "n" and "nwell" in self.layers:
            self.rect(builder, "nwell", -head, -300, length + 2 * head, width + 600)
        elif self.doping == "p" and "pwell" in self.layers:
            self.rect(builder, "pwell", -head, -300, length + 2 * head, width + 600)
        self.port(
            builder, "1", "met1", -head, -100, head + 100, width + 200, "inout", "left"
        )
        self.port(
            builder,
            "2",
            "met1",
            length - 100,
            -100,
            head + 100,
            width + 200,
            "inout",
            "right",
        )

    def build_mim_cap(self, builder):
        length = max(self.um("l", self.number("w", 2.0)), 500)
        width = max(self.um("h", self.number("w", 2.0)), 500)
        bottom = self.cap_bottom if self.cap_bottom in self.layers else "met1"
        top = self.cap_top if self.cap_top in self.layers else "met2"
        self.rect(builder, bottom, 0, 0, length, width)
        if self.cap_marker in self.layers:
            self.rect(
                builder,
                self.cap_marker,
                100,
                100,
                max(length - 200, 100),
                max(width - 200, 100),
            )
        self.rect(
            builder,
            top,
            200,
            200,
            max(length - 400, 100),
            max(width - 400, 100),
        )
        self.rect(builder, bottom, -200, width // 2 - 100, 200, 200)
        self.rect(builder, top, length, width // 2 - 100, 200, 200)
        self.port(
            builder, "1", bottom, -200, width // 2 - 100, 200, 200, "inout", "left"
        )
        self.port(
            builder, "2", top, length, width // 2 - 100, 200, 200, "inout", "right"
        )

    def build_vpp_cap(self, builder):
        length = max(self.um("l", self.number("w", 4.0)), 800)
        width = max(self.um("h", self.number("w", 4.0)), 800)
        for index, layer in enumerate(("met1", "met2", "met3", "met4", "met5")):
            if layer not in self.layers or index > 4:
                continue
            inset = index * 100
            self.rect(
                builder,
                layer,
                inset,
                inset,
                max(length - 2 * inset, 100),
                max(width - 2 * inset, 100),
            )
        self.rect(builder, "met1", -200, width // 2 - 100, 200, 200)
        top_layer = "met5" if "met5" in self.layers else "met4"
        self.rect(
            builder,
            top_layer,
            length,
            width // 2 - 100,
            200,
            200,
        )
        self.port(
            builder, "1", "met1", -200, width // 2 - 100, 200, 200, "inout", "left"
        )
        self.port(
            builder,
            "2",
            top_layer,
            length,
            width // 2 - 100,
            200,
            200,
            "inout",
            "right",
        )

    def build_var_cap(self, builder):
        length = max(self.um("l", 1.0), 100)
        width = max(self.um("w", 2.0), 200)
        self.rect(builder, "nwell", -300, -300, length + 600, width + 600)
        self.rect(builder, "diff", 0, 0, length, width)
        self.rect(builder, "poly", -length // 2, -200, length, width + 400)
        self.rect(builder, "li1", 100, width // 2 - 100, max(length - 200, 100), 200)
        self.rect(builder, "met1", -length // 2 - 100, -300, 100, width + 600)
        self.rect(builder, "met2", length // 2, -300, 100, width + 600)
        self.port(
            builder,
            "1",
            "met1",
            -length // 2 - 100,
            -300,
            100,
            width + 600,
            "inout",
            "left",
        )
        self.port(
            builder, "2", "met2", length // 2, -300, 100, width + 600, "inout", "right"
        )

    def build_diode(self, builder):
        width = max(self.um("w", 2.0), 300)
        height = max(self.um("h", self.number("w", 2.0)), 300)
        well = "nwell" if self.doping == "n" else "pwell"
        self.rect(builder, well, -300, -300, width + 600, height + 600)
        self.rect(builder, "diff", 0, 0, width, height)
        self.rect(builder, "li1", 100, height // 2 - 100, width - 200, 200)
        self.rect(builder, "met1", -300, height // 2 - 150, 300, 300)
        self.rect(builder, "met1", width, height // 2 - 150, 300, 300)
        if "nplus" in self.layers:
            self.rect(builder, "nplus", 0, 0, width // 2, height)
        if "pplus" in self.layers:
            self.rect(builder, "pplus", width // 2, 0, width // 2, height)
        self.port(
            builder, "A", "met1", -300, height // 2 - 150, 300, 300, "inout", "left"
        )
        self.port(
            builder, "K", "met1", width, height // 2 - 150, 300, 300, "inout", "right"
        )

    def build_bjt(self, builder):
        """Three terminal bipolar transistor with contacted base, emitter and collector.

        The device is a row of three diffusion taps (base, emitter, collector).
        Each tap gets licon1 contacts to li1, an mcon to met1 and a metal pad
        that carries the terminal, so the drawn geometry is a real connected
        device rather than a placeholder. Spacings follow the SKY130 diffusion
        and implant rules (0.28 um diff, 0.38 um implant, 0.17 um contacts).
        """
        cut = 170
        enclosure = 60
        tap_width = 400
        margin = 100
        gap = 700
        implant_enclosure = 125
        pad = 400
        size = self.um("w", 2.0)
        size = max(size, self.um("l", 2.0), 2 * margin + tap_width + 2 * gap + 400)
        pnp = self.doping == "p"
        base_implant = "nsdm" if pnp else "psdm"
        emitter_implant = "psdm" if pnp else "nsdm"

        columns = [margin + index * (tap_width + gap) for index in range(3)]
        height = size - 2 * margin
        # A substrate PNP keeps its collector in the p-substrate, so the nwell
        # only covers the base and emitter taps.
        covered = 2 if pnp else 3
        nwell_left = -enclosure
        nwell_right = columns[covered - 1] + tap_width + implant_enclosure + enclosure
        self.rect(
            builder,
            "nwell",
            nwell_left,
            -enclosure,
            nwell_right - nwell_left,
            size + 2 * enclosure,
        )

        for index, x in enumerate(columns):
            self.rect(builder, "diff", x, margin, tap_width, height)
            if "tap" in self.layers:
                self.rect(builder, "tap", x, margin, tap_width, height)
            implant = base_implant if index == 0 else emitter_implant
            self.rect(
                builder,
                implant,
                x - implant_enclosure,
                margin - implant_enclosure,
                tap_width + 2 * implant_enclosure,
                height + 2 * implant_enclosure,
            )
            # diffusion -> li1 contacts: two cuts per tap
            for fraction in (3, 5):
                self.rect(
                    builder,
                    "licon1",
                    x + tap_width // 2 - cut // 2,
                    margin + height * fraction // 8 - cut // 2,
                    cut,
                    cut,
                )
            self.rect(builder, "li1", x, margin, tap_width, height)
            # li1 -> met1 contact plus the terminal pad
            pad_x = x + tap_width // 2 - pad // 2
            pad_y = size // 2 - pad // 2
            self.rect(builder, "mcon", pad_x + pad // 2 - cut // 2, pad_y + pad // 2 - cut // 2, cut, cut)
            self.rect(builder, "met1", pad_x, pad_y, pad, pad)

        # Terminals are ordered base, collector, emitter so a diode connected
        # device can strap base and collector with one local patch.
        pads = [(x + tap_width // 2 - pad // 2, size // 2 - pad // 2) for x in columns]
        self.port(builder, "B", "met1", pads[0][0], pads[0][1], pad, pad, "input", "left")
        self.port(builder, "C", "met1", pads[1][0], pads[1][1], pad, pad, "inout", "top")
        self.port(builder, "E", "met1", pads[2][0], pads[2][1], pad, pad, "inout", "right")

    def build_via_stack(self, builder):
        width = max(self.um("w", 1.0), 200)
        height = max(self.um("h", 1.0), 200)
        for layer in self.stack_layers:
            if layer in self.layers:
                self.rect(builder, layer, 0, 0, width, height)
        for layer in ("met1", "met2", "met3", "met4"):
            if layer in self.layers:
                self.rect(builder, layer, width // 2 - 100, height // 2 - 100, 200, 200)

    def build_guard_ring(
        self, builder, x=0, y=0, width=2000, height=2000, thickness=300
    ):
        if width <= 0 or height <= 0:
            return
        self.rect(builder, "diff", x, y, width, thickness)
        self.rect(builder, "diff", x, y + height - thickness, width, thickness)
        self.rect(builder, "diff", x, y, thickness, height)
        self.rect(builder, "diff", x + width - thickness, y, thickness, height)
        if "nwell" in self.layers:
            self.rect(builder, "nwell", x - 200, y - 200, width + 400, height + 400)
        if "met1" in self.layers:
            self.rect(builder, "met1", x, y, width, thickness)
            self.rect(builder, "met1", x, y + height - thickness, width, thickness)

    def build_guard_ring_cell(self, builder):
        width = max(self.um("w", 20.0), 2000)
        height = max(self.um("h", 20.0), 2000)
        self.build_guard_ring(builder, 0, 0, width, height, max(width // 40, 300))

    def build_spiral(self, builder):
        turns = max(self.integer("turns", self.turns), 1)
        pitch = max(self.um("pitch", 2.0), 500)
        width = max(self.um("w", 1.0), 100)
        for turn in range(turns):
            offset = turn * pitch
            self.rect(builder, "met1", offset, offset, width, width)
            self.rect(builder, "met1", offset, offset, width, 100)
            self.rect(builder, "met2", offset + width, offset + width // 2, 100, 100)


def package_main_cli(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate PDK package PCell geometry through icepy"
    )
    parser.add_argument("--pdk-root", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--class", dest="class_name", required=True)
    parser.add_argument("--params-file")
    parser.add_argument("--output")
    parser.add_argument("--list-params", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.list_params:
            output = {
                "parameters": package_parameter_specs(
                    args.pdk_root, args.module, args.class_name
                )
            }
        else:
            overrides = {}
            if args.params_file:
                overrides = json.loads(Path(args.params_file).read_text())
                if not isinstance(overrides, dict):
                    raise RuntimeError("--params-file must contain a JSON object")
            output = generate_package_geometry(
                args.pdk_root, args.module, args.class_name, overrides
            )
        text = json.dumps(output, separators=(",", ":"))
        if args.output:
            Path(args.output).write_text(text)
        else:
            print(text)
        return 0
    except Exception as error:  # noqa: BLE001 - CLI boundary
        print(f"PCell generation failed: {error}", file=sys.stderr)
        return 1


__all__ = [
    "PACKAGE_DBU_PER_UM",
    "PackagePcell",
    "generate_package_geometry",
    "load_package_pcell_module",
    "package_main_cli",
    "package_parameter_specs",
    "package_parameter_values",
    "package_pcell_class",
]
