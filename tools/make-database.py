#!/usr/bin/env python3
"""
Infernal Unicode Database Generator
Genera data.txt completo a partir del UCD oficial (ReadMe, UnicodeData, emoji/*)
"""

import sys
import urllib.request
import re

# ----------------------------------------------------------------------
# URLs oficiales (latest)
# ----------------------------------------------------------------------
BASE_UCD = "https://www.unicode.org/Public/UCD/latest/"
README_URL       = BASE_UCD + "ReadMe.txt"
UNICODE_DATA_URL = BASE_UCD + "ucd/UnicodeData.txt"

BASE_EMOJI_UCD = BASE_UCD + "ucd/emoji/"
EMOJI_DATA_URL = BASE_EMOJI_UCD + "emoji-data.txt"

BASE_EMOJI = BASE_UCD + "emoji/"
EMOJI_SEQUENCES_URL = BASE_EMOJI + "emoji-sequences.txt"
EMOJI_ZWJ_SEQ_URL   = BASE_EMOJI + "emoji-zwj-sequences.txt"

# ----------------------------------------------------------------------
# Descarga
# ----------------------------------------------------------------------
def fetch_lines(url):
    try:
        with urllib.request.urlopen(url) as resp:
            return resp.read().decode("utf-8").splitlines()
    except Exception as e:
        print(f"Error descargando {url}: {e}", file=sys.stderr)
        sys.exit(1)

# ----------------------------------------------------------------------
# Versiones
# ----------------------------------------------------------------------
def get_unicode_version(readme_lines):
    """Lee la versión de Unicode desde ReadMe.txt."""
    pattern = re.compile(r"Unicode\s+(\d+\.\d+\.\d+)")
    for line in readme_lines:
        m = pattern.search(line)
        if m:
            return m.group(1)
    return "unknown"

def get_emoji_version(emoji_lines):
    for line in emoji_lines:
        if line.startswith("# Version:"):
            return line.split(":", 1)[1].strip()
    return "unknown"

# ----------------------------------------------------------------------
# Parseo de UnicodeData.txt
# ----------------------------------------------------------------------
def parse_unicode_data(lines):
    """
    Devuelve:
      - cp_to_name: dict int -> nombre oficial (generado para ideogramas)
      - cp_to_cat:  dict int -> categoría (Lu, Ll, Nd, ...)
      - case_pairs: list of (lower_cp, upper_cp) para BLOCK lower-upper
    """
    cp_to_name = {}
    cp_to_cat = {}
    case_pairs = []

    range_start = None
    range_cat = None
    range_prefix = None   # para construir nombres oficiales

    for line in lines:
        if not line.strip() or line.startswith("#"):
            continue

        parts = line.split(";")
        if len(parts) < 14:
            continue

        try:
            cp = int(parts[0], 16)
        except ValueError:
            continue

        name = parts[1]
        cat = parts[2]

        # Si es el inicio de un rango
        if name.endswith(", First>"):
            range_start = cp
            range_cat = cat
            # Prefijo para nombres: "CJK UNIFIED IDEOGRAPH EXTENSION A" etc.
            range_prefix = name[:-len(", First>")].strip()
            # El carácter que abre el rango sí tiene ese nombre de "First", pero
            # no lo usaremos para nombres porque no es un nombre real de carácter.
            # Lo guardamos provisionalmente, luego al cerrar el rango crearemos los verdaderos.
            continue

        # Si es el final del rango
        if name.endswith(", Last>") and range_start is not None:
            # Cerrar el rango generando nombres para todos los puntos intermedios
            for v in range(range_start, cp + 1):
                # Nombre oficial: prefijo + "-" + código en hex mayúsculas sin ceros extra
                official_name = f"{range_prefix}-{v:X}"
                cp_to_name[v] = official_name
                cp_to_cat[v] = range_cat
                # No hay case pairs en estos rangos (ideogramas)
            # Limpiar estado del rango
            range_start = None
            range_cat = None
            range_prefix = None
            continue

        # Carácter normal (fuera de rango)
        cp_to_name[cp] = name
        cp_to_cat[cp] = cat

        # Procesar case mappings: campo 12 (uppercase) y 13 (lowercase)
        # Solo nos interesa cuando hay una pareja simple (un solo carácter)
        # Formato de los campos: hexadecimal del código o vacío
        upper_hex = parts[12].strip()
        lower_hex = parts[13].strip()

        # Si el carácter es minúscula y tiene una mayúscula directa
        if cat == "Ll" and upper_hex:
            try:
                upper_cp = int(upper_hex, 16)
                case_pairs.append((cp, upper_cp))   # lower, upper
            except ValueError:
                pass
        # Si es mayúscula y tiene minúscula directa (redundante pero lo cubrimos)
        elif cat == "Lu" and lower_hex:
            try:
                lower_cp = int(lower_hex, 16)
                # Añadir solo si no se va a duplicar (podríamos usar un set, pero lo manejamos después)
                case_pairs.append((lower_cp, cp))
            except ValueError:
                pass

    return cp_to_name, cp_to_cat, case_pairs

# ----------------------------------------------------------------------
# Parseo de emoji-data.txt
# ----------------------------------------------------------------------
def parse_range(r):
    if ".." in r:
        s, e = r.split("..")
        return int(s, 16), int(e, 16)
    cp = int(r, 16)
    return cp, cp

def parse_property(lines, prop):
    cps = set()
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(";")
        if len(parts) < 2:
            continue
        p = parts[1].strip().split("#")[0].strip()
        if p == prop:
            start, end = parse_range(parts[0].strip())
            for cp in range(start, end+1):
                cps.add(cp)
    return cps

# ----------------------------------------------------------------------
# Parseo de secuencias
# ----------------------------------------------------------------------
def parse_sequences(lines):
    seqs = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(";")
        if len(parts) < 3:
            continue
        tipo = parts[1].strip().split("#")[0].strip()
        if not tipo.startswith("RGI_Emoji_"):
            continue
        cp_strs = parts[0].strip().split()
        try:
            cps = [int(c, 16) for c in cp_strs]
        except ValueError:
            continue
        emoji_str = "".join(chr(cp) for cp in cps)
        seqs.append((emoji_str, cps, tipo))
    return seqs

# ----------------------------------------------------------------------
# Escritura de bloques
# ----------------------------------------------------------------------
def write_cp_block(f, block_name, cps, ndict):
    """Escribe un bloque de caracteres individuales con nombre oficial."""
    f.write(f"BLOCK: {block_name}\n")
    for cp in sorted(cps):
        if cp not in ndict:      # sin nombre (no debería ocurrir con la generación actual)
            continue
        char = chr(cp)
        f.write(f"{char}-U+{cp:04X}-{ndict[cp].upper()}\n")
    f.write("\n")

def write_seq_block(f, block_name, seqs, ndict):
    """Escribe bloque de secuencias emoji."""
    f.write(f"BLOCK: {block_name}\n")
    for emoji_str, cps, tipo in seqs:
        hex_parts = " ".join(f"U+{cp:04X}" for cp in cps)
        names = [ndict.get(cp, f"<{cp:04X}>").upper() for cp in cps]
        f.write(f"{emoji_str}-{hex_parts}-{'-'.join(names)}\n")
    f.write("\n")

def write_case_block(f, pairs):
    """Genera BLOCK: lower-upper a partir de pares (lower_cp, upper_cp)."""
    f.write("BLOCK: lower-upper\n")
    # Ordenar por lower para estabilidad
    for low, up in sorted(pairs):
        low_char = chr(low)
        up_char = chr(up)
        f.write(f"{low_char}-{up_char}\n")
    f.write("\n")

# ----------------------------------------------------------------------
# Principal
# ----------------------------------------------------------------------
def generate():
    # 1. Leer ReadMe y versión
    print("Descargando ReadMe.txt ...")
    readme = fetch_lines(README_URL)
    unicode_version = get_unicode_version(readme)

    # 2. Procesar UnicodeData
    print("Descargando UnicodeData.txt ...")
    uni = fetch_lines(UNICODE_DATA_URL)
    cp_name, cp_cat, case_pairs = parse_unicode_data(uni)

    # 3. Procesar emoji-data
    print("Descargando emoji-data.txt ...")
    emoji_lines = fetch_lines(EMOJI_DATA_URL)
    emoji_version = get_emoji_version(emoji_lines)

    emoji       = parse_property(emoji_lines, "Emoji")
    emoji_pres  = parse_property(emoji_lines, "Emoji_Presentation")
    emoji_base  = parse_property(emoji_lines, "Emoji_Modifier_Base")
    emoji_mod   = parse_property(emoji_lines, "Emoji_Modifier")
    emoji_comp  = parse_property(emoji_lines, "Emoji_Component")

    # 4. Secuencias
    print("Descargando secuencias...")
    seq_lines = fetch_lines(EMOJI_SEQUENCES_URL)
    seqs = parse_sequences(seq_lines)
    zwj_lines = fetch_lines(EMOJI_ZWJ_SEQ_URL)
    zwj_seqs = parse_sequences(zwj_lines)

    # 5. Conjuntos de letras y números (categorías)
    all_letters = {cp for cp, cat in cp_cat.items() if cat.startswith("L")}
    all_numbers = {cp for cp, cat in cp_cat.items() if cat == "Nd"}

    # 6. Escribir data.txt
    with open("data.txt", "w", encoding="utf-8") as f:
        f.write(f"Unicode Version: {unicode_version}\n")
        f.write(f"# Generated by Infernal Unicode Database Generator\n")
        f.write(f"# Emoji Version: {emoji_version}\n\n")

        # Bloque lower-upper automático
        write_case_block(f, case_pairs)

        # Bloques emoji
        write_cp_block(f, "emoji-codepoints", emoji, cp_name)
        write_cp_block(f, "emoji-presentation", emoji_pres, cp_name)
        write_cp_block(f, "emoji-modifier-bases", emoji_base, cp_name)
        write_cp_block(f, "emoji-modifiers", emoji_mod, cp_name)
        write_cp_block(f, "emoji-components", emoji_comp, cp_name)

        write_seq_block(f, "emoji-sequences", seqs, cp_name)
        write_seq_block(f, "emoji-zwj-sequences", zwj_seqs, cp_name)

        write_cp_block(f, "all-letters", all_letters, cp_name)
        write_cp_block(f, "all-number", all_numbers, cp_name)

    print(f"data.txt generado: Unicode {unicode_version}, Emoji {emoji_version}")
    print(f"  Letras: {len(all_letters)}  |  Números: {len(all_numbers)}")
    print(f"  Pares mayús/min: {len(case_pairs)}")
    print(f"  Emoji simples: {len(emoji)}  |  Secuencias: {len(seqs)}  |  ZWJ: {len(zwj_seqs)}")

if __name__ == "__main__":
    generate()
