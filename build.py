#!/bin/python

import argparse
import os
import platform
import subprocess
import sys


class CmakeTarget:
    """
    handles all cmake configuration and build target
    """

    def __init__(self, build_dir="build"):
        self.default_tool = "cmake"
        self.build_dir = build_dir

    @staticmethod
    def config():
        return {
            "configure": {
                "cmd": ["-S", ".", "-G", "Ninja"],
                "description": "Configure the project",
                "build_dir": "-B",
            },
            "build": {
                "cmd": [],
                "description": "Build the project",
                "build_dir": "--build",
            },
            "db": {
                "cmd": ["-S", ".", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
                "description": "Generate compile_commands.json",
                "build_dir": "-B",
            },
            "refresh": {
                "cmd": ["--fresh", "-S", ".", "-G", "Ninja"],
                "description": "Fresh configure the project",
                "build_dir": "-B",
            },
            "clean": {
                "cmd": ["--target", "clean"],
                "description": "Clean the build files",
                "build_dir": "--build",
                "post": CmakeTarget.clean_artifact,
            },
            "test": {
                "cmd": ["--output-on-failure"],
                "description": "Run tests",
                "build_dir": "--build",
                "tool": "ctest",
            },
        }

    @staticmethod
    def cmd():
        return CmakeTarget.config().keys()

    @staticmethod
    def help():
        lines = []
        for mode, info in CmakeTarget.config().items():
            lines.append(f"  {mode:<10} - {info['description']}")
        return "\n".join(lines)

    @staticmethod
    def clean_artifact():
        if platform.system() == "Windows":
            MSVC_TEMP_EXTS = [".ilk", ".pdb"]

            for file in os.listdir("."):
                if any(file.endswith(ext) for ext in MSVC_TEMP_EXTS):
                    os.remove(file)

    def invoke(self, mode):
        cmds = [self.generate_cmd_mode(mode)]
        run_cmd(cmds)

        post_hook = self.config()[mode].get("post")
        if callable(post_hook):
            post_hook()

    def generate_cmd_mode(self, mode):
        cfg = self.config()[mode]
        cmd = cfg["cmd"].copy()

        cmd.insert(0, cfg.get("tool", self.default_tool))

        # If 'build_dir' flag is provided
        build_flag = cfg.get("build_dir")
        if build_flag is not None:
            cmd[1:1] = [build_flag, self.build_dir]

        return cmd

    def generate_all(self):
        return {mode: self.generate_cmd_mode(mode) for mode in self.config()}


def run_cmd(cmds: list):
    for cmd in cmds:
        try:
            subprocess.run(cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Error: Command failed with exit code {e.returncode}")
            sys.exit(e.returncode)


def main():
    parser = argparse.ArgumentParser(
        prog="build.py",
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser.add_argument(
        "--build-dir",
        default="build",
        help="Specify custom build directory (default: build)",
    )

    parser.add_argument(
        "mode",
        metavar="mode",
        choices=CmakeTarget.cmd(),
        help=CmakeTarget.help(),
    )

    args = parser.parse_args()
    cmake_target = CmakeTarget(args.build_dir)
    cmake_target.invoke(args.mode)


if __name__ == "__main__":
    main()
