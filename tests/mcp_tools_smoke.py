#!/usr/bin/env python3
"""Smoke-test the ice-mcp schematic, simulation, and diagnostics tools.

Run after building ice-mcp and ice-cli:
    python3 tests/mcp_tools_smoke.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICE_MCP = ROOT / "build/linux/x86_64/release/ice-mcp"
ICE_CLI = ROOT / "build/linux/x86_64/release/ice-cli"


class MCP:
    def __init__(self, project: Path):
        self.process = subprocess.Popen(
            [str(ICE_MCP), "--project", str(project)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.next_id = 1
        self.call(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "mcp-tools-smoke", "version": "1"},
            },
        )

    def call(self, method: str, params: dict) -> dict:
        self.next_id += 1
        request = {
            "jsonrpc": "2.0",
            "id": self.next_id,
            "method": method,
            "params": params,
        }
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError(
                    "ice-mcp closed unexpectedly:\n" + self.process.stderr.read()
                )
            message = json.loads(line)
            if message.get("id") != self.next_id:
                continue
            if "error" in message:
                raise RuntimeError(json.dumps(message["error"]))
            return message["result"]

    def tool(self, tool_name: str, **arguments) -> dict:
        result = self.call("tools/call", {"name": tool_name, "arguments": arguments})
        text = result["content"][0]["text"]
        if result.get("isError"):
            raise RuntimeError(text)
        return json.loads(text)

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def main() -> int:
    if not ICE_MCP.is_file() or not ICE_CLI.is_file():
        raise SystemExit("Build ice-mcp and ice-cli before running this smoke test.")

    cli_version = json.loads(subprocess.check_output([str(ICE_CLI), "--version"]))
    assert cli_version.get("commit"), cli_version
    assert cli_version.get("build_time"), cli_version
    assert "project_repair" in cli_version.get("features", []), cli_version

    with tempfile.TemporaryDirectory(prefix="ice-mcp-smoke-") as temporary:
        project = Path(temporary) / "project"
        subprocess.run(
            [str(ICE_CLI), "project-create", str(project), "MCP Smoke"],
            check=True,
            capture_output=True,
        )

        mcp = MCP(project)
        try:
            tools = {tool["name"] for tool in mcp.call("tools/list", {})["tools"]}
            for required in {
                "ice_doctor",
                "schematic_list",
                "schematic_info",
                "schematic_add_symbol",
                "schematic_add_pin",
                "schematic_add_net",
                "schematic_netlist",
                "project_repair",
                "simulation_result",
                "simulation_signals",
                "simulation_measure",
                "pdk_info",
                "pdk_build",
                "project_migrate",
                "schematic_add_text",
                "layout_add_instance",
            }:
                if required not in tools:
                    raise RuntimeError(f"Missing MCP tool: {required}")

            version = mcp.tool("ice_version")
            assert version.get("commit"), version
            assert version.get("build_time"), version
            assert "project_repair" in version.get("features", []), version
            assert "pdk_build" in version.get("features", []), version

            mcp.tool("schematic_create", name="ring_test", pdk="sky130")
            for symbol in ("inv1", "inv2"):
                mcp.tool(
                    "schematic_add_symbol",
                    cell="ring_test",
                    name=symbol,
                    pins=[
                        {"name": "A", "x": 0, "y": 0, "direction": "left"},
                        {"name": "Y", "x": 100, "y": 0, "direction": "right"},
                    ],
                )
            mcp.tool(
                "schematic_add_instance",
                cell="ring_test",
                symbol="inv1",
                name="I1",
                x=0,
                y=0,
            )
            mcp.tool(
                "schematic_add_instance",
                cell="ring_test",
                symbol="inv2",
                name="I2",
                x=200,
                y=0,
            )
            mcp.tool(
                "schematic_add_net", cell="ring_test", name="n1", pins=["I1.Y", "I2.A"]
            )

            info = mcp.tool("schematic_info", schematic="ring_test")
            assert len(info["symbols"]) == 2
            assert len(info["instances"]) == 2
            assert len(info["nets"]) == 1
            assert info["symbols"][0]["pins"]
            mcp.tool("schematic_netlist", schematic="ring_test")

            # Regression: the advertised 'schematic' key must work without
            # also supplying the legacy 'cell' key.
            text_cell = mcp.tool(
                "schematic_create_text",
                name="text_test",
                text="* rc\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.tran 1n 1u\n.end\n",
            )
            mcp.tool(
                "schematic_set_text",
                schematic=text_cell["id"],
                text="* rc updated\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.tran 1n 1u\n.end\n",
            )
            kind_text = mcp.tool(
                "schematic_create",
                name="kind_text",
                kind="text",
                text="* kind text\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            assert kind_text["kind"] == "text", kind_text
            alias_text = mcp.tool(
                "schematic_add_text",
                name="alias_text",
                text="* alias text\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            assert alias_text["kind"] == "text", alias_text
            mcp.tool("schematic_create", name="convert_me")
            converted = mcp.tool(
                "schematic_set_text",
                schematic="convert_me",
                text="* converted\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
                convert=True,
            )
            assert converted["converted"] is True, converted

            mcp.tool(
                "simulation_create",
                name="rc_sim",
                schematic="text_test",
                analysis="tran",
                stop=1e-6,
                step=1e-9,
                signals="in,out",
            )
            run = mcp.tool("simulation_run", simulation="rc_sim")
            assert run["state"] == "succeeded", run
            assert run["trace_count"] >= 2, run
            assert "available_signals" in run and "netlist" in run
            stored = mcp.tool("simulation_result", simulation="rc_sim")
            assert stored["state"] == "succeeded", stored
            signals = mcp.tool("simulation_signals", simulation="rc_sim")
            assert {"in", "out"}.issubset(set(signals["available_signals"]))
            measured = mcp.tool("simulation_measure", simulation="rc_sim", signal="out")
            assert measured["measurements"], measured
            assert "zero_crossings" in measured["measurements"][0], measured
            assert "amplitude" in measured["measurements"][0], measured

            pdk = mcp.tool("pdk_info")
            assert "pdks" in pdk, pdk

            migrated = mcp.tool("project_migrate")
            assert migrated.get("guidance_version") == 2, migrated

            doctor = mcp.tool("ice_doctor")
            assert "pdk_roots" in doctor
            assert "pdks" in doctor
            assert "simulation_model_libraries" in doctor
            assert "timing_libraries" in doctor
            assert "model_libraries" in doctor
            assert "ngspice_executable_available" in doctor
            assert "build" in doctor and doctor["build"].get("commit"), doctor

            # Recovery: a malformed sidecar file is quarantined instead of
            # preventing the project from opening.
            (project / "sch" / "broken.icesch").write_text(
                '{"type":"text","pdk":"generic","text":"* broken"}'
            )
            reopened = mcp.tool("project_open", path=str(project), recover=True)
            assert reopened.get("quarantined_files"), reopened
            assert not (project / "sch" / "broken.icesch").exists()

            (project / "sch" / "broken2.icesch").write_text(
                '{"type":"text","pdk":"generic","text":"* broken again"}'
            )
            repaired = mcp.tool("project_repair")
            assert repaired.get("repaired") is True, repaired
            assert repaired.get("quarantined_files"), repaired

            # Project-aware model validation reports missing include files.
            mcp.tool(
                "schematic_create_text",
                name="bad_model",
                text='* bad model\n.include "/tmp/ice-does-not-exist-model.lib"\n'
                "V1 in 0 1\nR1 in out 1k\nC1 out 0 1p\n.end\n",
            )
            mcp.tool("simulation_create", name="bad_sim", schematic="bad_model")
            bad_doctor = mcp.tool("ice_doctor", simulation="bad_sim")
            assert bad_doctor["model_paths_ok"] is False, bad_doctor
            assert any(not check["exists"] for check in bad_doctor["model_checks"])

            print("ice-mcp tool smoke tests passed")
            return 0
        finally:
            mcp.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"ice-mcp tool smoke tests failed: {error}", file=sys.stderr)
        sys.exit(1)
