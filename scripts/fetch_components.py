import configparser
import subprocess
from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR")).resolve()
CONFIG_FILE = PROJECT_DIR / "platformio.ini"


def run(command, cwd=None, check=True):
    """Run a command, displaying it in a PlatformIO-friendly form."""

    location = Path(cwd) if cwd else PROJECT_DIR

    print(
        "[StopWatch dependencies] "
        + " ".join(str(argument) for argument in command)
    )

    return subprocess.run(
        [str(argument) for argument in command],
        cwd=str(location),
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def git_output(component_dir, *arguments):
    result = run(
        ["git", *arguments],
        cwd=component_dir,
    )

    return result.stdout.strip()


def ensure_component(
    name,
    relative_path,
    repository,
    ref,
    patch_path=None,
    recursive=False,
):
    component_dir = PROJECT_DIR / relative_path

    print(
        f"[StopWatch dependencies] Checking {name} "
        f"({repository} @ {ref})"
    )

    if not component_dir.exists():
        component_dir.parent.mkdir(
            parents=True,
            exist_ok=True,
        )

        clone_command = [
            "git",
            "clone",
            "--depth",
            "1",
            "--branch",
            ref,
        ]

        if recursive:
            clone_command.extend(
                [
                    "--recurse-submodules",
                    "--shallow-submodules",
                ]
            )

        clone_command.extend(
            [
                repository,
                str(component_dir),
            ]
        )

        result = run(
            clone_command,
            cwd=PROJECT_DIR,
            check=False,
        )

        if result.returncode != 0:
            print(result.stdout)
            raise RuntimeError(
                f"Unable to clone dependency '{name}'"
            )

    git_dir = component_dir / ".git"

    if not git_dir.exists():
        raise RuntimeError(
            f"{component_dir} exists but is not a Git repository"
        )

    configured_origin = git_output(
        component_dir,
        "remote",
        "get-url",
        "origin",
    )

    if configured_origin != repository:
        raise RuntimeError(
            f"Dependency '{name}' has unexpected origin:\n"
            f"  expected: {repository}\n"
            f"  actual:   {configured_origin}"
        )

    current_commit = git_output(
        component_dir,
        "rev-parse",
        "HEAD",
    )

    ref_result = run(
        [
            "git",
            "rev-parse",
            f"{ref}^{{commit}}",
        ],
        cwd=component_dir,
        check=False,
    )

    if ref_result.returncode != 0:
        fetch_result = run(
            [
                "git",
                "fetch",
                "--depth",
                "1",
                "origin",
                ref,
            ],
            cwd=component_dir,
            check=False,
        )

        if fetch_result.returncode != 0:
            print(fetch_result.stdout)
            raise RuntimeError(
                f"Unable to fetch '{ref}' for dependency '{name}'"
            )

        desired_commit = git_output(
            component_dir,
            "rev-parse",
            "FETCH_HEAD",
        )

    else:
        desired_commit = ref_result.stdout.strip()

    if current_commit != desired_commit:
        print(
            f"[StopWatch dependencies] Updating {name} to {ref}"
        )

        fetch_result = run(
            [
                "git",
                "fetch",
                "--depth",
                "1",
                "origin",
                ref,
            ],
            cwd=component_dir,
            check=False,
        )

        if fetch_result.returncode != 0:
            print(fetch_result.stdout)
            raise RuntimeError(
                f"Unable to update dependency '{name}'"
            )

        run(
            [
                "git",
                "checkout",
                "--force",
                "FETCH_HEAD",
            ],
            cwd=component_dir,
        )

    if recursive:
        run(
            [
                "git",
                "submodule",
                "update",
                "--init",
                "--recursive",
                "--depth",
                "1",
            ],
            cwd=component_dir,
        )

    if patch_path:
        patch_file = PROJECT_DIR / patch_path

        if not patch_file.exists():
            raise RuntimeError(
                f"Patch for dependency '{name}' does not exist: "
                f"{patch_file}"
            )

        # First determine whether the patch has already been applied.
        reverse_check = run(
            [
                "git",
                "apply",
                "--reverse",
                "--check",
                str(patch_file),
            ],
            cwd=component_dir,
            check=False,
        )

        if reverse_check.returncode == 0:
            print(
                f"[StopWatch dependencies] "
                f"Patch already applied to {name}"
            )

        else:
            apply_check = run(
                [
                    "git",
                    "apply",
                    "--check",
                    str(patch_file),
                ],
                cwd=component_dir,
                check=False,
            )

            if apply_check.returncode != 0:
                print(apply_check.stdout)

                raise RuntimeError(
                    f"Patch cannot be applied to dependency '{name}'"
                )

            print(
                f"[StopWatch dependencies] Applying patch to {name}"
            )

            run(
                [
                    "git",
                    "apply",
                    str(patch_file),
                ],
                cwd=component_dir,
            )


def load_components():
    config = configparser.ConfigParser(
        interpolation=None,
    )

    config.read(
        CONFIG_FILE,
        encoding="utf-8",
    )

    if "m5_components" not in config:
        raise RuntimeError(
            "Missing [m5_components] section in platformio.ini"
        )

    return config["m5_components"]


components = load_components()

for component_name, specification in components.items():
    fields = [
        field.strip()
        for field in specification.split("|")
    ]

    while len(fields) < 5:
        fields.append("")

    (
        relative_path,
        repository,
        ref,
        patch_path,
        recursive_value,
    ) = fields[:5]

    recursive = recursive_value.lower() in (
        "1",
        "true",
        "yes",
        "on",
    )

    ensure_component(
        component_name,
        relative_path,
        repository,
        ref,
        patch_path or None,
        recursive,
    )

print("[StopWatch dependencies] All components ready")