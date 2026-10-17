# Pre-build hook for every env: firmware version header and path scrubbing.
#
# Version: FW_VERSION comes from the BC250_FW_VERSION environment variable
# (scripts/publish_site.py sets it for release builds) and is "dev" otherwise.
# It lives in a generated header rewritten only when it changes, so a dev build
# does not recompile everything and only the files that include it rebuild.
#
# Paths: __FILE__ in the framework sources (assert/log macros) embeds absolute
# paths such as C:/Users/<name>/.platformio/... into firmware.bin. Released
# binaries go to other people, so map the home and project directories away.
Import("env")
import os
from pathlib import Path

version = os.environ.get("BC250_FW_VERSION", "dev")
gen_dir = Path(env.subst("$BUILD_DIR")) / "generated"
gen_dir.mkdir(parents=True, exist_ok=True)
header = gen_dir / "fw_version.h"
content = '#pragma once\n#define FW_VERSION "%s"\n' % version
if not header.exists() or header.read_text() != content:
    header.write_text(content)
env.Append(CPPPATH=[str(gen_dir)])

prefixes = [(Path.home(), "~"), (Path(env.subst("$PROJECT_DIR")), ".")]
flags = []
for path, replacement in prefixes:
    # GCC sees both spellings on Windows; map each.
    for spelling in {str(path), path.as_posix()}:
        flags.append("-fmacro-prefix-map=%s=%s" % (spelling, replacement))
env.Append(CCFLAGS=flags)
