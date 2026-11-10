#!/usr/bin/env python3
"""Build authored Vulkan/Metal probes; staging and board execution are separate."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("vulkan", "metal"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vulkan-headers", type=Path,
                        help="Include directory containing vulkan/vulkan.h")
    parser.add_argument("--sdk", default="iphoneos", choices=("iphoneos", "macosx"))
    parser.add_argument("--test", choices=("all", "framebuffer-fetch", "readonly", "shared-atomic"), default="all")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / "gpu" / args.kind
    out = args.output.resolve()
    if out.exists():
        parser.error("output directory must be new; existing evidence is preserved")
    if args.kind == "vulkan" and (not args.vulkan_headers or
            not (args.vulkan_headers / "vulkan/vulkan.h").is_file()):
        parser.error("Vulkan needs --vulkan-headers containing vulkan/vulkan.h")
    out.mkdir(parents=True)
    commands = []

    def run(command):
        command = [str(word) for word in command]
        commands.append(command)
        completed = subprocess.run(command, capture_output=True, text=True)
        with (out / "build.log").open("a") as log:
            log.write(json.dumps(command) + "\n" + completed.stdout + completed.stderr)
        completed.check_returncode()
        return completed.stdout

    sdk = run(["xcrun", "--sdk", args.sdk, "--show-sdk-path"]).strip()
    target = "arm64-apple-ios17.0" if args.sdk == "iphoneos" else "arm64-apple-macos14.0"
    common = ["-target", target, "-isysroot", sdk, "-O2", "-Wall", "-Wextra", "-Werror"]
    if args.test == "framebuffer-fetch":
        if args.kind != "metal":
            parser.error("framebuffer-fetch is a Metal probe mode")
        common.append("-DE5_MTL_FETCH_TEST_ONLY=1")
    if args.test == "readonly":
        if args.kind != "metal":
            parser.error("readonly is a Metal probe mode")
        common.append("-DE5_MTL_READONLY_TEST_ONLY=1")
    if args.test == "shared-atomic":
        if args.kind != "metal":
            parser.error("shared-atomic is a Metal probe mode")
        common.append("-DE5_MTL_ATOMIC_TEST_ONLY=1")
    if args.kind == "vulkan":
        for name in ("e5vk-probe.c", "render_probe.c", "probe.h"):
            shutil.copy2(source / name, out / name)
        for filename in ("square.comp", "triangle.vert", "yellow.frag"):
            binary = out / (filename + ".spv")
            run(["glslangValidator", "-V", "--target-env", "vulkan1.1",
                 source / filename, "-o", binary])
            run(["spirv-val", "--target-env", "vulkan1.1", binary])
            data = binary.read_bytes()
            if not data or len(data) % 4:
                raise ValueError("invalid SPIR-V word count")
            words = struct.unpack("<" + str(len(data) // 4) + "I", data)
            name = filename.split(".")[0]
            rows = [", ".join(f"0x{word:08x}u" for word in words[i:i + 8]) + ","
                    for i in range(0, len(words), 8)]
            (out / (name + ".inc")).write_text(
                f"static const uint32_t {name}_spv[] = {{\n" + "\n".join(rows) + "\n};\n")
        executable = out / "e5vk-probe"
        run(["xcrun", "--sdk", args.sdk, "clang", *common,
             "-I" + str(args.vulkan_headers.resolve()), out / "e5vk-probe.c",
             out / "render_probe.c", "-o", executable])
    else:
        air_target = target.replace("arm64-", "air64-", 1)
        language = "ios-metal2.4" if args.sdk == "iphoneos" else "macos-metal2.4"
        run(["xcrun", "--sdk", args.sdk, "metal", "-target", air_target,
             "-std=" + language, "-c", source / "probe.metal", "-o", out / "probe.air"])
        run(["xcrun", "--sdk", args.sdk, "metallib", out / "probe.air",
             "-o", out / "probe.metallib"])
        executable = out / "e5metal-probe"
        run(["xcrun", "--sdk", args.sdk, "clang++", *common, "-std=c++17", "-fobjc-arc",
             source / "e5metal-probe.mm", "-framework", "Foundation", "-framework", "Metal",
             "-framework", "IOKit", "-framework", "IOSurface", "-o", executable])
    run(["codesign", "-f", "-s", "-", executable])
    run(["codesign", "--verify", "--strict", executable])
    signature = subprocess.run(["codesign", "-dvvv", str(executable)],
                               capture_output=True, text=True, check=True).stderr
    manifest = dict(kind=args.kind, test=args.test, target=target, sdk=sdk, commands=commands,
                    executable=executable.name, executable_sha256=sha(executable),
                    cdhash=re.search(r"CDHash=(\w+)", signature)[1],
                    sources={p.name: sha(p) for p in source.iterdir() if p.is_file()},
                    generated={p.name: sha(p) for p in out.iterdir()
                               if p.suffix in (".spv", ".air", ".metallib")},
                    signature_verified=True, hardware_execution=False)
    (out / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({key: manifest[key] for key in
                      ("kind", "target", "executable_sha256", "cdhash", "signature_verified")}, indent=2))


if __name__ == "__main__":
    main()
