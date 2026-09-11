#!/usr/bin/env python3
"""Fetch/check pinned sources, or deliver mem_sim without a GitHub login.

Only creates new destination trees; existing trees are checked, never reset.
The mem_sim archive contains committed build sources, without .git or credentials.
The upstream long-form manual is excluded; project guides document this workflow.
"""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LOCK = json.loads((ROOT / "upstream.lock.json").read_text())
RECEIPT = ".het-upstream.json"


def paths():
    workspace = ROOT.parent
    return {
        "gem5": Path(os.environ.get("GEM5_HOME", workspace / "gem5")).resolve(),
        "vortex": Path(os.environ.get("VORTEX_HOME", workspace / "vortex-gpu/vortex")).resolve(),
        "coralnpu": Path(os.environ.get("CORALNPU_HOME", workspace / "coralnpu")).resolve(),
        "memsim": Path(os.environ.get("MEMSIM_HOME", workspace / "mem_sim")).resolve(),
    }


def git(path, *args, network=False):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never")
    command = ["git", "-C", str(path)]
    if network:
        # Do not invoke GUI sign-in or a stored credential helper. An authorized
        # mirror/local path or the offline archive can be supplied instead.
        command += ["-c", "credential.helper=", "-c", "core.askPass="]
        env["GIT_ASKPASS"] = "/bin/false"
        env["SSH_ASKPASS"] = "/bin/false"
    result = subprocess.run(command + list(args), env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # Remote URLs can contain credentials. Do not echo the command/stderr.
        raise ValueError("Git 操作失败 (%s)。核对路径、固定 revision 与访问权限；"
                         "mem_sim 可用 --memsim-archive 离线导入。" % args[0])
    return result.stdout.strip()


def revision(path):
    if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
        raise ValueError("不是独立 Git 源码树: %s" % path)
    return git(path, "rev-parse", "HEAD")


def check(name, path):
    pin = LOCK[name]
    if name == "memsim" and (path / RECEIPT).is_file():
        receipt = json.loads((path / RECEIPT).read_text())
        if (receipt.get("revision") != pin["revision"] or
                not isinstance(receipt.get("files"), list) or not receipt["files"]):
            raise ValueError("mem_sim 源码包 receipt 与 lock 不一致")
        for filename in receipt["files"]:
            target = (path / filename).resolve()
            if path.resolve() not in target.parents or not target.is_file():
                raise ValueError("mem_sim 源码包文件缺失/路径无效: %s" % filename)
        print("ok memsim snapshot %s (%d files)" % (pin["revision"], len(receipt["files"])))
        return
    actual = revision(path)
    if name == "gem5" and actual == pin["legacy_revision"]:
        print("WARN gem5 使用历史私有 fork %s；新安装应使用官方 %s" %
              (actual, pin["revision"]))
    elif actual != pin["revision"]:
        raise ValueError("%s revision 期望 %s，实际 %s；请使用新目录，脚本不会切换已有树" %
                         (name, pin["revision"], actual))
    else:
        print("ok %s revision %s" % (name, actual))
    for relative, expected in pin.get("submodules", {}).items():
        if revision(path / relative) != expected:
            raise ValueError("Vortex submodule revision 不匹配: %s" % relative)
        print("ok %s %s" % (relative, expected))


def import_memsim(archive, destination):
    pin = LOCK["memsim"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".het-memsim-", dir=destination.parent) as tmp:
        staging = Path(tmp) / "source"
        staging.mkdir()
        files = []
        with tarfile.open(archive, "r:gz") as packed:
            # git archive records its source commit in the global pax header.
            # This identifies the declared version, not file-content integrity.
            if packed.pax_headers.get("comment") != pin["revision"]:
                raise ValueError("mem_sim 源码包声明的 Git commit 与 lock 不一致；未导入")
            for member in packed.getmembers():
                parts = PurePosixPath(member.name).parts
                if (not parts or parts[0] != "mem_sim" or ".." in parts or
                        not (member.isdir() or member.isfile())):
                    raise ValueError("源码包包含不支持的路径/文件类型")
                target = staging.joinpath(*parts[1:])
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                filename = str(target.relative_to(staging))
                if filename == RECEIPT or filename in files:
                    raise ValueError("源码包包含重复文件或保留文件名")
                if filename in pin.get("excluded_delivery_files", []):
                    raise ValueError("源码包含未交付的上游说明；请用本项目重新导出的附件")
                target.parent.mkdir(parents=True, exist_ok=True)
                with packed.extractfile(member) as source, target.open("wb") as output:
                    output.write(source.read())
                target.chmod(member.mode & 0o777)
                files.append(filename)
        if not files:
            raise ValueError("源码包没有普通文件")
        receipt = dict(revision=pin["revision"], files=sorted(files),
                       excluded_delivery_files=pin.get("excluded_delivery_files", []))
        (staging / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n")
        if destination.exists():
            raise ValueError("目标已存在，未覆盖: %s" % destination)
        staging.rename(destination)
    check("memsim", destination)


def fetch(name, destination, args):
    if destination.exists():
        check(name, destination)
        return
    if name == "memsim" and args.memsim_archive:
        import_memsim(args.memsim_archive.resolve(), destination)
        return
    url = args.memsim_url if name == "memsim" and args.memsim_url else LOCK[name]["url"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    # A failed clone must not leave a destination that looks installed.
    with tempfile.TemporaryDirectory(prefix=".het-fetch-", dir=destination.parent) as tmp:
        staging = Path(tmp) / "source"
        staging.mkdir()
        git(staging, "init", "--quiet")
        git(staging, "remote", "add", "origin", url)
        print("fetch %s @ %s" % (name, LOCK[name]["revision"]), flush=True)
        git(staging, "fetch", "--depth=1", "origin", LOCK[name]["revision"], network=True)
        git(staging, "checkout", "--detach", "FETCH_HEAD")
        if LOCK[name].get("submodules"):
            git(staging, "submodule", "update", "--init", "--recursive", network=True)
        check(name, staging)
        if destination.exists():
            raise ValueError("目标已存在，未覆盖: %s" % destination)
        staging.rename(destination)
    print("installed %s -> %s" % (name, destination))


def package_memsim(source, output):
    pin = LOCK["memsim"]
    revision(source)  # Require a real source repository, not an enclosing repo.
    output = output.resolve()
    if output.exists():
        raise ValueError("输出已存在，未覆盖；请使用新的 --output: %s" % output)
    output.parent.mkdir(parents=True, exist_ok=True)
    tracked = git(source, "ls-tree", "-rz", "--name-only", pin["revision"]).split("\0")
    included = [name for name in tracked
                if name and name not in pin.get("excluded_delivery_files", [])]
    if not included:
        raise ValueError("没有可交付的源码文件")
    with tempfile.TemporaryDirectory(prefix=".het-package-", dir=output.parent) as tmp:
        archive = Path(tmp) / "source.tar.gz"
        git(source, "archive", "--format=tar.gz", "--prefix=mem_sim/",
            "--output=" + str(archive), pin["revision"], "--", *included)
        archive.rename(output)
    print("packaged memsim @ %s -> %s" % (pin["revision"], output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "check", "package-memsim"))
    parser.add_argument("--only", choices=tuple(LOCK))
    parser.add_argument("--memsim-archive", type=Path)
    parser.add_argument("--memsim-url", help="已授权的镜像/本地 Git 路径，不要嵌入 token")
    parser.add_argument("--output", type=Path, default=ROOT / "build/deps" /
                        ("mem_sim-%s.tar.gz" % LOCK["memsim"]["revision"]))
    args = parser.parse_args()
    trees = paths()
    if args.action == "package-memsim":
        package_memsim(trees["memsim"], args.output)
        return 0
    failures = 0
    for name in ([args.only] if args.only else LOCK):
        try:
            if args.action == "fetch":
                fetch(name, trees[name], args)
            else:
                check(name, trees[name])
        except (ValueError, OSError) as error:
            print("FAIL %s: %s" % (name, error), file=sys.stderr)
            failures += 1
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, tarfile.TarError) as error:
        print("错误: %s" % error, file=sys.stderr)
        sys.exit(1)
