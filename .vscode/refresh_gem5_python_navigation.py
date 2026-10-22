#!/usr/bin/env python3
"""Map gem5's dynamic m5.objects imports to their local source definitions."""

import ast
import json
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GEM5_SRC = ROOT / "vortex_StorageStacked/third_party/gem5/src"
HOST_ISA = json.loads((ROOT / "vortex_StorageStacked/user/smoke/config.json").read_text())["host"]["isa"]
ISA_MODULE = "X86CPU" if HOST_ISA == "x86_64" else "ArmCPU"
OUTPUT = ROOT / ".vscode/gem5-python-stubs/m5/objects"


def parse(path):
    return ast.parse(path.read_text(), filename=str(path))


def registered_modules():
    modules = {}
    for script in sorted(GEM5_SRC.rglob("SConscript")):
        for node in ast.walk(parse(script)):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                continue
            argument = None
            if node.func.id == "SimObject" and node.args:
                argument = node.args[0]
            elif node.func.id == "PySource" and len(node.args) > 1:
                if isinstance(node.args[0], ast.Constant) and node.args[0].value == "m5.objects":
                    argument = node.args[1]
            if not isinstance(argument, ast.Constant) or not isinstance(argument.value, str):
                continue
            source = script.parent / argument.value
            if source.suffix != ".py" or not source.is_file():
                continue
            if source.stem in modules and modules[source.stem] != source:
                raise ValueError(f"ambiguous m5.objects module: {source.stem}")
            modules[source.stem] = source
    modules["SimObject"] = GEM5_SRC / "python/m5/SimObject.py"
    modules["StorageBridge"] = ROOT / "vortex_StorageStacked/integration/StorageBridge.py"
    modules["VortexGPGPU"] = ROOT / "vortex_StorageStacked/third_party/vortex/sim/simx/gem5/VortexGPGPU.py"
    return modules


def source_module(source):
    if GEM5_SRC / "python" in source.parents:
        relative = source.relative_to(GEM5_SRC / "python")
    elif GEM5_SRC in source.parents:
        relative = source.relative_to(GEM5_SRC)
    else:
        relative = source.relative_to(ROOT)
    return ".".join(relative.with_suffix("").parts)


def definitions(source, modules):
    tree = parse(source)
    result = {
        node.name: (source_module(source), node.name)
        for node in tree.body
        if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
        and not node.name.startswith("_")
    }
    if not result and "arch_vars" in source.read_text():
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.module == "m5.objects." + ISA_MODULE:
                for name in node.names:
                    if name.asname:
                        result[name.asname] = (source_module(modules[ISA_MODULE]), name.name)
    return result


def required_imports():
    imports = defaultdict(set)
    sources = [*sorted((ROOT / "vortex_StorageStacked/third_party/gem5/runtime").glob("*.py")), ROOT / "vortex_StorageStacked/integration/StorageBridge.py", ROOT / "vortex_StorageStacked/third_party/vortex/sim/simx/gem5/VortexGPGPU.py"]
    for source in sources:
        for node in ast.walk(parse(source)):
            if isinstance(node, ast.ImportFrom) and node.module:
                if node.module == "m5.objects" or node.module.startswith("m5.objects."):
                    module = node.module[len("m5.objects"):].lstrip(".")
                    imports[module].update(name.name for name in node.names if name.name != "*")
    return imports


def write_stub(path, exports):
    text = "# Generated for editor navigation; definitions remain in the source tree.\n"
    text += "".join(f"from {module} import {original} as {name}\n"
                    for name, (module, original) in sorted(exports.items()))
    path.write_text(text)


def main():
    configuration = ROOT / "vortex_StorageStacked/third_party/gem5/build" / ("AXI" if HOST_ISA == "x86_64" else "AXI_ARM") / "gem5.build/config"
    if not configuration.is_file() or ("USE_X86_ISA=y" if HOST_ISA == "x86_64" else "USE_ARM_ISA=y") not in configuration.read_text().splitlines():
        raise SystemExit("Selected gem5 ISA configuration is missing; build the platform first")
    modules = registered_modules()
    exports = {name: definitions(source, modules) for name, source in modules.items()}
    symbols = defaultdict(list)
    for module_exports in exports.values():
        for name, target in module_exports.items():
            if target not in symbols[name]:
                symbols[name].append(target)
    aggregate = {name: targets[0] for name, targets in symbols.items() if len(targets) == 1}
    for params_file in (GEM5_SRC / "python/m5/params").glob("*.py"):
        aggregate.update(definitions(params_file, modules))

    missing = []
    for module, names in required_imports().items():
        available = exports.get(module, {}) if module else aggregate
        missing.extend(f"m5.objects.{module}.{name}" for name in sorted(names) if name not in available)
    if missing:
        raise SystemExit("Python navigation mappings are missing: " + ", ".join(missing))

    OUTPUT.mkdir(parents=True, exist_ok=True)
    for module, module_exports in sorted(exports.items()):
        write_stub(OUTPUT / f"{module}.pyi", module_exports)
    write_stub(OUTPUT / "__init__.pyi", aggregate)
    report = {
        "gem5_isa": HOST_ISA,
        "module_count": len(exports),
        "export_count": len(aggregate),
        "required_imports": {module or "__init__": sorted(names)
                             for module, names in required_imports().items()},
        "ambiguous_exports": {name: targets for name, targets in symbols.items() if len(targets) > 1},
        "missing_imports": missing,
    }
    (ROOT / ".vscode/gem5_python_navigation_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"gem5 Python navigation: {len(exports)} modules, {len(aggregate)} exports, no missing project imports")


if __name__ == "__main__":
    main()
