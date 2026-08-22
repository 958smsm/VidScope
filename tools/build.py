#!/usr/bin/env python3
"""
VidScope Centralized Build Runner (build.py)

Cross-platform build script for VidScope. Handles MSVC environment setup on Windows,
CMake configuration, building targets, running tests, and deployment/installation.
"""

import sys
import os
import shutil
import subprocess
import argparse
import json
from pathlib import Path


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def setup_msvc_env_if_needed():
    """On Windows, if cl.exe is missing from PATH, attempt to locate and import VsDevShell environment."""
    if sys.platform != "win32":
        return

    if shutil.which("cl.exe"):
        return  # MSVC compiler already available in PATH

    print("[INFO] MSVC compiler (cl.exe) not found in PATH. Locating Visual Studio installation...")
    
    devshell_script = None

    # 1. Use vswhere.exe if available
    vswhere_exe = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere_exe.exists():
        try:
            cmd = [
                str(vswhere_exe),
                "-latest",
                "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property", "installationPath"
            ]
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            vs_path_str = res.stdout.strip()
            if vs_path_str:
                cand_script = Path(vs_path_str) / "Common7" / "Tools" / "Launch-VsDevShell.ps1"
                if cand_script.exists():
                    devshell_script = cand_script
        except Exception as e:
            print(f"[DEBUG] vswhere lookup failed: {e}")

    # 2. Fallback search if vswhere didn't find it
    if not devshell_script:
        vs_search_roots = [
            Path(r"E:\Program Files\Microsoft Visual Studio"),
            Path(r"D:\Program Files\Microsoft Visual Studio"),
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio",
            Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio",
        ]
        for root in vs_search_roots:
            if root.exists():
                scripts = sorted(root.glob("**/Launch-VsDevShell.ps1"), reverse=True)
                if scripts:
                    devshell_script = scripts[0]
                    break

    if not devshell_script or not devshell_script.exists():
        print("[WARN] Could not find Launch-VsDevShell.ps1. Proceeding with default PATH environment.")
        return

    print(f"[INFO] Found VS DevShell at {devshell_script}. Capturing environment...")
    ps_cmd = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command",
        f"& '{devshell_script}' -Arch amd64 -SkipAutomaticLocation; Get-ChildItem env: | ForEach-Object {{ \"$($_.Name)=$($_.Value)\" }}"
    ]

    try:
        res = subprocess.run(ps_cmd, capture_output=True, text=True, check=True)
        env_count = 0
        for line in res.stdout.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                os.environ[key] = value
                env_count += 1
        if shutil.which("cl.exe"):
            print(f"[INFO] Successfully initialized MSVC x64 developer environment ({env_count} variables updated).")
        else:
            print("[WARN] VS DevShell script ran but cl.exe was still not found in PATH.")
    except Exception as e:
        print(f"[WARN] Failed to execute VS DevShell script: {e}")


def load_presets(repo_root: Path):
    presets_file = repo_root / "CMakePresets.json"
    if not presets_file.exists():
        return None
    try:
        with open(presets_file, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def run_command(cmd, cwd: Path) -> int:
    print(f"\n[EXEC] {subprocess.list2cmdline(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(cmd, cwd=cwd)
    return res.returncode


def main():
    repo_root = get_repo_root()

    # Delegate 'doctor' command
    if len(sys.argv) > 1 and sys.argv[1] in ("doctor", "--doctor"):
        doctor_py = repo_root / "tools" / "doctor.py"
        res = subprocess.run([sys.executable, str(doctor_py)] + sys.argv[2:])
        return res.returncode

    parser = argparse.ArgumentParser(
        description="Centralized cross-platform build script for VidScope.",
        add_help=True
    )
    parser.add_argument("--preset", type=str, help="CMake preset name (e.g. windows-msvc, windows-debug, windows-release)")
    parser.add_argument("-c", "--config", type=str, default="Release", choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"], help="Build configuration (default: Release)")
    parser.add_argument("-b", "--build-dir", type=str, help="Build output directory (default: build)")
    parser.add_argument("--clean", action="store_true", help="Clean build directory before configuring")
    parser.add_argument("-t", "--target", type=str, help="Specific build target to compile")
    parser.add_argument("-T", "--test", action="store_true", help="Run CTest tests after building")
    parser.add_argument("--test-preset", type=str, help="Specific test preset to execute")
    parser.add_argument("--install", action="store_true", help="Install targets after building")
    parser.add_argument("--prefix", type=str, help="Custom installation prefix directory")
    parser.add_argument("--qt-dir", type=str, help="Path to Qt 6 prefix directory")
    parser.add_argument("--ffmpeg-dir", type=str, help="Path to FFmpeg SDK root directory")
    parser.add_argument("-G", "--generator", type=str, help="CMake generator name")
    parser.add_argument("-j", "--jobs", type=int, help="Number of parallel compilation jobs")
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose output during build/test")
    parser.add_argument("--no-deploy", action="store_true", help="Disable Qt/FFmpeg runtime DLL deployment on Windows")
    parser.add_argument("--no-tests", action="store_true", help="Disable building tests")

    args, extra_args = parser.parse_known_args()

    # Ensure MSVC environment is set up on Windows if needed
    setup_msvc_env_if_needed()

    presets_data = load_presets(repo_root)

    # Determine if preset mode is used
    preset_name = args.preset
    configure_preset = None
    build_preset = None
    test_preset = args.test_preset

    if preset_name and presets_data:
        # Check if preset_name is configurePreset or buildPreset
        config_names = [p.get("name") for p in presets_data.get("configurePresets", [])]
        build_names = {p.get("name"): p.get("configurePreset") for p in presets_data.get("buildPresets", [])}
        test_names = {p.get("name"): p for p in presets_data.get("testPresets", [])}

        if preset_name in build_names:
            build_preset = preset_name
            configure_preset = build_names[preset_name]
            if not test_preset and preset_name in test_names:
                test_preset = preset_name
        elif preset_name in config_names:
            configure_preset = preset_name
        else:
            print(f"[WARN] Unknown preset '{preset_name}'. Proceeding with raw preset argument.")
            configure_preset = preset_name

    # Handle cleaning build directory
    if args.clean:
        target_build_dir = Path(args.build_dir) if args.build_dir else repo_root / "build"
        if target_build_dir.exists():
            print(f"[INFO] Cleaning build directory: {target_build_dir}")
            shutil.rmtree(target_build_dir, ignore_errors=True)

    # Configure stage
    if configure_preset:
        cmake_config_cmd = ["cmake", "--preset", configure_preset]
    else:
        build_dir = args.build_dir or "build"
        cmake_config_cmd = ["cmake", "-S", ".", "-B", build_dir]
        if args.generator:
            cmake_config_cmd.extend(["-G", args.generator])
        cmake_config_cmd.append(f"-DCMAKE_BUILD_TYPE={args.config}")

        qt_dir = args.qt_dir or os.environ.get("CMAKE_PREFIX_PATH")
        if qt_dir:
            cmake_config_cmd.append(f"-DCMAKE_PREFIX_PATH={qt_dir}")

        ffmpeg_dir = args.ffmpeg_dir or os.environ.get("FFMPEG_ROOT")
        if ffmpeg_dir:
            cmake_config_cmd.append(f"-DFFMPEG_ROOT={ffmpeg_dir}")

        if args.no_deploy:
            cmake_config_cmd.append("-DVIDSCOPE_DEPLOY_RUNTIME=OFF")
        if args.no_tests:
            cmake_config_cmd.append("-DVIDSCOPE_BUILD_TESTS=OFF")

    if extra_args:
        cmake_config_cmd.extend(extra_args)

    code = run_command(cmake_config_cmd, cwd=repo_root)
    if code != 0:
        print(f"[ERROR] CMake configuration failed with exit code {code}")
        return code

    # Build stage
    if build_preset:
        cmake_build_cmd = ["cmake", "--build", "--preset", build_preset]
    else:
        build_dir = args.build_dir or "build"
        cmake_build_cmd = ["cmake", "--build", build_dir, "--config", args.config]

    if args.target:
        cmake_build_cmd.extend(["--target", args.target])
    if args.jobs:
        cmake_build_cmd.extend(["--parallel", str(args.jobs)])
    if args.verbose:
        cmake_build_cmd.append("--verbose")

    code = run_command(cmake_build_cmd, cwd=repo_root)
    if code != 0:
        print(f"[ERROR] CMake build failed with exit code {code}")
        return code

    # Test stage
    if args.test:
        if test_preset:
            ctest_cmd = ["ctest", "--preset", test_preset]
        else:
            build_dir = args.build_dir or "build"
            ctest_cmd = ["ctest", "--test-dir", build_dir, "-C", args.config, "--output-on-failure"]
        if args.verbose:
            ctest_cmd.append("--verbose")

        code = run_command(ctest_cmd, cwd=repo_root)
        if code != 0:
            print(f"[ERROR] CTest test execution failed with exit code {code}")
            return code

    # Install stage
    if args.install:
        build_dir = args.build_dir or "build"
        cmake_install_cmd = ["cmake", "--install", build_dir, "--config", args.config]
        if args.prefix:
            cmake_install_cmd.extend(["--prefix", args.prefix])

        code = run_command(cmake_install_cmd, cwd=repo_root)
        if code != 0:
            print(f"[ERROR] CMake install failed with exit code {code}")
            return code

    print("\n[SUCCESS] VidScope build workflow completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
