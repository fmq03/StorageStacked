#!/usr/bin/env python3
"""Export native CoralNPU compile actions using its existing Bazel environment."""
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'coralnpu_StorageStacked/coralnpu/runtime'))
from native_build import bazel
from configure import load_config
from rtl_build import variant_name
variant=variant_name(load_config(ROOT/'coralnpu_StorageStacked/user/smoke/config.json'))
actions=bazel('aquery',[f'mnemonic("CppCompile", deps(//native:{variant}.so))','--output=jsonproto'],True)
(ROOT/'.vscode/bazel_actions.json').write_text(actions)
print('Bazel compile actions:',len(json.loads(actions).get('actions',[])))
