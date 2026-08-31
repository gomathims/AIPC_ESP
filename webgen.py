#
# webgen.py — PlatformIO pre-build hook.
#
# Walks data/ and generates src/web_content.h (PROGMEM strings + byte arrays)
# and src/web_mappings.h (the stringMap / binaryMap the web server looks up).
#
# Deliberately dependency-free: unlike the ESP32 project's generator it does NOT
# require htmlmin / csscompressor / jsmin. Assets are embedded verbatim (only
# ##VERSION## is substituted and CRs stripped), which costs a little extra flash
# but keeps the build portable. The ESP8266 has 4 MB flash, so size is fine.
#
import os
import mimetypes

Import("env")

WEB_DIR = "data"
OUT_CONTENT = "src/web_content.h"
OUT_MAP = "src/web_mappings.h"


def get_version():
    """Build 'M.m.p.b' from the -D FIRMWARE_* flags + an incrementing build number."""
    defines = {}
    for item in env.get("CPPDEFINES", []):
        if isinstance(item, (list, tuple)) and len(item) == 2:
            defines[str(item[0])] = str(item[1]).strip('"')
    major = defines.get("FIRMWARE_MAJOR", "1")
    minor = defines.get("FIRMWARE_MINOR", "0")
    patch = defines.get("FIRMWARE_PATCH", "0")

    build_file = "build_number.txt"
    n = 0
    if os.path.exists(build_file):
        try:
            with open(build_file) as f:
                n = int(f.read().strip())
        except (ValueError, OSError):
            n = 0
    n += 1
    with open(build_file, "w") as f:
        f.write(str(n))
    return "%s.%s.%s.%s" % (major, minor, patch, n)


def var_name(rel_path):
    return os.path.splitext(rel_path.replace("/", "_").replace(".", "_"))[0]


def escape_text(s):
    return (s.replace("\r", "")
             .replace("\\", "\\\\")
             .replace('"', '\\"')
             .replace("\n", '\\n"\n"'))


def generate(version):
    text_map = {}    # rel_path -> C declaration
    binary_map = {}  # rel_path -> {"data": decl, "len": decl}

    for root, _, files in os.walk(WEB_DIR):
        for name in files:
            fpath = os.path.join(root, name)
            rel = os.path.relpath(fpath, WEB_DIR).replace("\\", "/")
            mime, _ = mimetypes.guess_type(fpath)
            vn = var_name(rel)

            is_text = mime and (mime.startswith("text")
                                or mime == "application/javascript"
                                or mime == "image/svg+xml")
            is_bin = mime and mime.startswith("image") and not is_text

            if is_text:
                with open(fpath, "r", encoding="utf-8") as f:
                    content = f.read()
                content = content.replace("##VERSION##", version)
                text_map[rel] = 'const char %s[] PROGMEM = "%s";' % (vn, escape_text(content))
            elif is_bin:
                with open(fpath, "rb") as f:
                    data = f.read()
                arr = ", ".join("0x%02x" % b for b in data)
                binary_map[rel] = {
                    "data": "const unsigned char %s[] PROGMEM = { %s };" % (vn, arr),
                    "len": "const unsigned int %s_len = %d;" % (vn, len(data)),
                }

    # ---- web_content.h ----
    out = ["#ifndef WEB_CONTENT_H", "#define WEB_CONTENT_H", ""]
    out.append("// Text assets (HTML/CSS/JS/SVG)")
    for rel, decl in sorted(text_map.items()):
        out.append("// %s" % rel)
        out.append(decl)
        out.append("")
    out.append("// Binary assets (PNG/ICO)")
    for rel, d in sorted(binary_map.items()):
        out.append("// %s" % rel)
        out.append(d["data"])
        out.append(d["len"])
        out.append("")
    out.append("#endif // WEB_CONTENT_H")
    write_if_changed(OUT_CONTENT, "\n".join(out) + "\n")

    # ---- web_mappings.h ----
    m = ["#ifndef WEB_MAPPINGS_H", "#define WEB_MAPPINGS_H", "",
         "#include <map>", "#include <Arduino.h>", '#include "web_content.h"', ""]
    m.append("std::map<String, const char*> stringMap = {")
    for rel in sorted(text_map):
        m.append('    {"%s", %s},' % (rel, var_name(rel)))
    m.append("};")
    m.append("")
    m.append("struct BinaryFile {")
    m.append("    const unsigned char* data;")
    m.append("    unsigned int length;")
    m.append("};")
    m.append("")
    m.append("std::map<String, BinaryFile> binaryMap = {")
    for rel in sorted(binary_map):
        vn = var_name(rel)
        m.append('    {"%s", {%s, %s_len}},' % (rel, vn, vn))
    if "images/favicon.ico" in binary_map:
        m.append('    {"favicon.ico", {images_favicon_ico, images_favicon_ico_len}},')
    m.append("};")
    m.append("")
    m.append("#endif // WEB_MAPPINGS_H")
    write_if_changed(OUT_MAP, "\n".join(m) + "\n")


def write_if_changed(path, content):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == content:
                return
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("webgen: wrote %s" % path)


version = get_version()
print("webgen: firmware version %s" % version)
generate(version)
