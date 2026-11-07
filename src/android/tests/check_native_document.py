#!/usr/bin/env python3
"""Run the dependency-free checks against Gradle's compiled NativeDocument.

First run example/android/gradlew :react-native-downstream_core:compileDebugKotlin
from example/android. The runner reuses Kotlin artifacts already in Gradle's
cache and installs no dependencies.
"""

import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
classes = root / "src/android/build/tmp/kotlin-classes/debug"
if not (classes / "com/downstream/NativeDocument.class").exists():
    raise SystemExit("Build :react-native-downstream_core:compileDebugKotlin in example/android first")
cache = Path(os.environ.get("GRADLE_USER_HOME", str(Path.home() / ".gradle"))) / "caches/modules-2/files-2.1"


def artifact(group, name):
    jars = list((cache / group / name).glob("*/*/*.jar"))
    if not jars:
        raise SystemExit(f"Missing cached {name}; build the example Android project first")
    return max(jars, key=lambda jar: tuple(int(n) for n in jar.parents[1].name.split(".") if n.isdigit()))


stdlib = artifact("org.jetbrains.kotlin", "kotlin-stdlib")
compiler = [
    artifact("org.jetbrains.kotlin", "kotlin-compiler-embeddable"),
    stdlib,
    artifact("org.jetbrains.kotlin", "kotlin-script-runtime"),
    artifact("org.jetbrains.kotlin", "kotlin-reflect"),
    artifact("org.jetbrains.intellij.deps", "trove4j"),
    artifact("org.jetbrains.kotlinx", "kotlinx-coroutines-core-jvm"),
    artifact("org.jetbrains", "annotations"),
]
with tempfile.TemporaryDirectory(prefix="downstream-android-check-") as temporary:
    subprocess.run([
        "java", "-cp", os.pathsep.join(map(str, compiler)),
        "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect",
        "-classpath", os.pathsep.join(map(str, [classes, stdlib])),
        f"-Xfriend-paths={classes}", "-d", temporary,
        str(Path(__file__).with_name("NativeDocumentRegression.kt")),
    ], check=True)
    subprocess.run([
        "java", "-cp", os.pathsep.join(map(str, [temporary, classes, stdlib])),
        "com.downstream.NativeDocumentRegressionKt",
    ], check=True)
