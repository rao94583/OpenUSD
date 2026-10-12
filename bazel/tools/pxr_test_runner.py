#!/usr/bin/env python3
"""Runs one pxr_register_test under Bazel (see //bazel:pxr_test.bzl).

CMake runs tests from its install tree: commands refer to
${CMAKE_INSTALL_PREFIX}/tests/<program> and .../bin/<tool>, and test scripts
find test plugins relative to themselves. This runner assembles that
<prefix> in the test's temporary directory from runfiles:

  <prefix>/lib, <prefix>/plugin   -> //bazel/install (libusd_ms, resources,
                                     the pxr Python package)
  <prefix>/bin/<tool>             -> the tools the test uses
  <prefix>/tests/...              -> the tests/ trees of the packages involved

and then runs cmake/macros/testWrapper.py with the arguments
pxr_register_test (cmake/macros/Public.cmake) would give it.

Usage: pxr_test_runner.py <spec.json>
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile


def _link_merged(sources, dest):
    """Makes dest show the union of the directories in sources."""
    sources = [s for s in sources if os.path.isdir(s)]
    if not sources:
        os.makedirs(dest)
    elif len(sources) == 1:
        os.symlink(sources[0], dest)
    else:
        os.makedirs(dest)
        entries = {}
        for source in sources:
            for entry in os.listdir(source):
                entries.setdefault(entry, []).append(
                    os.path.join(source, entry))
        for entry, paths in entries.items():
            target = os.path.join(dest, entry)
            if len(paths) == 1:
                os.symlink(paths[0], target)
            elif all(os.path.isdir(p) for p in paths):
                _link_merged(paths, target)
            else:
                sys.exit("pxr_test_runner: %s is provided by several test "
                         "trees: %s" % (entry, paths))


def _install_python_tool(source, dest):
    """Installs a Python program like pxr_python_bin: the source with its
    /pxrpythonsubst shebang pointing at this interpreter."""
    with open(source) as f:
        lines = f.readlines()
    if lines and lines[0].startswith("#!"):
        lines[0] = lines[0].replace("/pxrpythonsubst", sys.executable)
    with open(dest, "w") as f:
        f.writelines(lines)
    os.chmod(dest, 0o755)


def _make_prefix(spec, root, prefix):
    os.makedirs(prefix)
    install = os.path.join(root, "bazel/install")
    for name in ("lib", "plugin"):
        os.symlink(os.path.join(install, name), os.path.join(prefix, name))
    _link_merged([os.path.join(root, d) for d in spec["tests_dirs"]],
                 os.path.join(prefix, "tests"))
    bin_dir = os.path.join(prefix, "bin")
    os.makedirs(bin_dir)
    for name, path in spec["tools"].items():
        os.symlink(os.path.join(root, path), os.path.join(bin_dir, name))
    for name, path in spec["python_tools"].items():
        _install_python_tool(os.path.join(root, path),
                             os.path.join(bin_dir, name))


def _writable_copy(src, dest):
    """Copies a testenv directory, making the copy writable.

    testWrapper.py copies the testenv into the test's run directory keeping
    file modes; Bazel outputs are read-only, but tests may modify their
    testenv files, as they can in CMake's install tree.
    """
    shutil.copytree(src, dest, symlinks=False)
    for directory, _, files in os.walk(dest):
        os.chmod(directory, 0o755)
        for f in files:
            path = os.path.join(directory, f)
            os.chmod(path, os.stat(path).st_mode | 0o200)


def _wrapper_arguments(name, a, python_path, testenv_copy, image_diff_dir):
    """testWrapper.py arguments, in the order pxr_register_test adds them."""
    args = ["--verbose"]
    for key, flag in [
        ("STDOUT_REDIRECT", "--stdout-redirect"),
        ("STDERR_REDIRECT", "--stderr-redirect"),
        ("PRE_COMMAND_STDOUT_REDIRECT", "--pre-command-stdout-redirect"),
        ("PRE_COMMAND_STDERR_REDIRECT", "--pre-command-stderr-redirect"),
        ("POST_COMMAND_STDOUT_REDIRECT", "--post-command-stdout-redirect"),
        ("POST_COMMAND_STDERR_REDIRECT", "--post-command-stderr-redirect"),
    ]:
        if a.get(key):
            args.append("%s=%s" % (flag, a[key]))

    # With TESTENV_DEST the testenv is installed under a directory named
    # after the test; otherwise under the testenv's own name.
    if a.get("TESTENV") and not a.get("TESTENV_DEST"):
        testenv_dir = "<prefix>/tests/ctest/" + os.path.basename(a["TESTENV"])
    else:
        testenv_dir = "<prefix>/tests/ctest/" + name
    testenv_dir = testenv_copy(testenv_dir)
    args.append("--testenv-dir=" + testenv_dir)

    for f in a.get("DIFF_COMPARE", []):
        args.append("--diff-compare=" + f)
    # IMAGE_DIFF_COMPARE runs only with IMAGE_DIFF_TOOL (idiff).
    if a.get("IMAGE_DIFF_COMPARE") and image_diff_dir:
        for f in a["IMAGE_DIFF_COMPARE"]:
            args.append("--image-diff-compare=" + f)
        for key, flag in [
            ("WARN", "--warn"),
            ("WARN_PERCENT", "--warnpercent"),
            ("HARD_WARN", "--hardwarn"),
            ("FAIL", "--fail"),
            ("FAIL_PERCENT", "--failpercent"),
            ("HARD_FAIL", "--hardfail"),
        ]:
            if a.get(key):
                args.append("%s=%s" % (flag, a[key]))
        if a.get("PERCEPTUAL"):
            args.append("--perceptual")
        # So testWrapper.py finds idiff on PATH.
        args.append("--post-path=" + image_diff_dir)
    if a.get("DIFF_COMPARE") or a.get("IMAGE_DIFF_COMPARE"):
        args.append("--baseline-dir=%s/baseline" % testenv_dir)
        args.append("--failures-dir=<build>/Testing/Failed-Diffs/"
                    "<PXR_CTEST_RUN_ID>/" + name)
    for key, flag in [
        ("CLEAN_OUTPUT", "--clean-output-paths"),
        ("FILES_EXIST", "--files-exist"),
        ("FILES_DONT_EXIST", "--files-dont-exist"),
        ("PRE_COMMAND", "--pre-command"),
        ("POST_COMMAND", "--post-command"),
        ("EXPECTED_RETURN_CODE", "--expected-return-code"),
    ]:
        # if(bt_X) in CMake: "0" and "" are false.
        if a.get(key) not in (None, "", "0"):
            args.append("%s=%s" % (flag, a[key]))

    args.append("--env-var=TF_FATAL_VERIFY=1")
    for env in a.get("ENV", []):
        args.append("--env-var=" + env)
    for path in a.get("PRE_PATH", []):
        args.append("--pre-path=" + path)
    for path in a.get("POST_PATH", []):
        args.append("--post-path=" + path)
    args.append("--env-var=PYTHONPATH=<prefix>/lib/python:" + python_path)

    if a.get("CUSTOM_PYTHON"):
        command = "%s %s" % (a["CUSTOM_PYTHON"], a["COMMAND"])
    elif a.get("PYTHON"):
        command = "<python> " + a["COMMAND"]
    else:
        command = a["COMMAND"]
    return args + [command]


def main():
    with open(sys.argv[1]) as f:
        spec = json.load(f)

    runfiles = os.environ.get("RUNFILES_DIR") or os.environ["TEST_SRCDIR"]
    root = os.path.join(runfiles, "_main")
    tmp = os.environ.get("TEST_TMPDIR") or tempfile.mkdtemp()
    prefix = os.path.join(tmp, "prefix")
    if os.path.exists(prefix):
        shutil.rmtree(prefix)
    _make_prefix(spec, root, prefix)

    # Python packages CMake's interpreter provides (FindJinja2.cmake,
    # FindPySide.cmake, FindPyOpenGL.cmake); //bazel:python_deps.
    python_path = ":".join(
        os.path.dirname(os.path.dirname(importlib.util.find_spec(m).origin))
        for m in ("jinja2", "markupsafe", "PySide6", "shiboken6", "OpenGL"))

    build = os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", tmp)
    substitutions = {
        "<prefix>": prefix,
        "<python>": sys.executable,
        "<build>": build,
    }

    def substitute(arg):
        for placeholder, value in substitutions.items():
            arg = arg.replace(placeholder, value)
        return arg

    def testenv_copy(testenv_dir):
        src = substitute(testenv_dir)
        if not os.path.isdir(src):
            return testenv_dir  # No testenv; testWrapper skips the copy.
        dest = os.path.join(tmp, "testenv")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        _writable_copy(src, dest)
        return dest

    image_diff_dir = None
    if spec.get("image_diff_tool"):
        image_diff_dir = os.path.join(tmp, "image_diff")
        os.makedirs(image_diff_dir, exist_ok=True)
        link = os.path.join(image_diff_dir, "idiff")
        if not os.path.exists(link):
            os.symlink(os.path.join(root, spec["image_diff_tool"]), link)

    args = [substitute(arg) for arg in _wrapper_arguments(
        spec["name"], spec["arguments"], python_path, testenv_copy,
        image_diff_dir)]

    wrapper = os.path.join(root, "cmake/macros/testWrapper.py")
    env = dict(os.environ, TMPDIR=tmp)
    # rules_python sets PYTHONSAFEPATH, which keeps a script's directory off
    # sys.path; test scripts import helpers installed next to them.
    env.pop("PYTHONSAFEPATH", None)
    sys.stdout.flush()
    sys.exit(subprocess.call([sys.executable, wrapper] + args, env=env))


if __name__ == "__main__":
    main()
