"""Checks the cut-down menu in the source, so no board is needed: the menu
and Tools rows, and that ESP-MESH and the routed relay are not started."""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
MENU = ["flood_page", "lora_page", "broadcast_page", "lora_broadcast_page",
        "status", "tools_page"]
TOOLS = ["tool_rate", "tool_mute_flood", "tool_mute_lora"]


def code(name: str) -> str:
    """The file without its // comments."""
    return re.sub(r"//[^\n]*", "", (SRC / name).read_text())


def main() -> int:
    failures = 0

    def expect(ok: bool, what: str) -> None:
        nonlocal failures
        if not ok:
            failures += 1
            print(f"FAIL: {what}")

    ui = code("ui.inc")
    menu = re.search(r"if \(list == screen::menu\)\s*\{(.*?)\}\s*else", ui,
                     re.S)
    rows = re.findall(r"row_kind::(\w+), 0\}", menu.group(1)) if menu else []
    expect(rows == MENU, f"menu rows are {MENU} (got {rows})")

    tools = re.search(r"const row_kind tools\[\] = \{(.*?)\};", ui, re.S)
    switches = re.findall(r"row_kind::(\w+)", tools.group(1)) if tools else []
    expect(switches == TOOLS, f"Tools switches are {TOOLS} (got {switches})")

    main_cpp = code("main.cpp")
    for call in ("setup_mesh", "service_mesh", "run_mesh_command",
                 "print_mesh"):
        expect(not re.search(rf"\b{call}\(", main_cpp),
               f"main.cpp doesn't call {call}(): ESP-MESH isn't started")

    relay = code("relay.inc")
    expect("relay_nets[] = {&flood_net};" in relay,
           "only the flood mesh is set up and serviced")
    expect("routed_net.muted = true;" in relay, "the routed relay is muted")

    expect('"ESP-NOW flood"' in code("handset.inc"),
           'the flood row is labelled "ESP-NOW flood"')

    if failures:
        print(f"{failures} cut-down check(s) failed")
        return 1
    print("All cut-down menu tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
