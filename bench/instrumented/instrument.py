#!/usr/bin/env python3
"""regenerate the instrumented copy of flexigif's lzw encoder.

the copy is derived from third-party sources that carry no license grant
("copyright (c) 2018 stephan brumme. all rights reserved."), so it is not
committed. run this once against a chisel checkout to rebuild it locally:

    python3 bench/instrumented/instrument.py ../chisel
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent

HEADER_PATCH = [(
    "#include <cstdint>",
    """#include <cstdint>

// counters used to attribute the pre-pass cost
struct LzwStats
{
  unsigned long long calls, clearedEntries, tokens, matchSteps, addCodeSteps, bytesScanned, bestUpdates;
  double clearMs;
};
extern LzwStats g_lzwStats;""",
)]

SOURCE_PATCH = [
    ("#include <iostream>", "#include <iostream>\n#include <chrono>\n\nLzwStats g_lzwStats{};"),
    ("""    uint8_t oneByte = m_data[from++];
    code = m_dictionary[code][oneByte];
  }""",
     """    uint8_t oneByte = m_data[from++];
    code = m_dictionary[code][oneByte];
    g_lzwStats.addCodeSteps++;
  }"""),
    ("""    const uint8_t oneByte = m_data[from++];
    code = m_dictionary[code][oneByte];
    // no continuation => return number of matching byte
    if (code == Unknown)
      return length;""",
     """    const uint8_t oneByte = m_data[from++];
    code = m_dictionary[code][oneByte];
    g_lzwStats.matchSteps++;
    // no continuation => return number of matching byte
    if (code == Unknown)
      return length;"""),
    ("""  // Ensure m_dictionary is sized to m_maxDictionary and initialized
  if (m_dictionary.size() < m_maxDictionary)""",
     """  g_lzwStats.calls++;
  g_lzwStats.clearedEntries += m_dirtySize;
  const auto clearStart = std::chrono::steady_clock::now();

  // Ensure m_dictionary is sized to m_maxDictionary and initialized
  if (m_dictionary.size() < m_maxDictionary)"""),
    ("""  if (m_isGif)
    m_dictSize = clear + 2;""",
     """  g_lzwStats.clearMs += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - clearStart).count();

  if (m_isGif)
    m_dictSize = clear + 2;"""),
    ("""      // counters
      numBits += codeSize;
      numTokens++;""",
     """      // counters
      numBits += codeSize;
      numTokens++;
      g_lzwStats.tokens++;"""),
    ("""    // "eat" next byte of the current match""",
     """    g_lzwStats.bytesScanned++;

    // "eat" next byte of the current match"""),
    ("""      best.bits      = trueBits;""",
     """      g_lzwStats.bestUpdates++;
      best.bits      = trueBits;"""),
]


def patch(text, edits, name):
    for old, new in edits:
        if old not in text:
            sys.exit(f"{name}: anchor not found, flexigif has changed:\n{old[:70]}")
        text = text.replace(old, new, 1)
    return text


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    core = pathlib.Path(sys.argv[1]).expanduser() / "third_party" / "flexigif" / "core"
    if not (core / "LzwEncoder.cpp").exists():
        sys.exit(f"no flexigif sources under {core}")

    header = (core / "LzwEncoder.hpp").read_text()
    source = (core / "LzwEncoder.cpp").read_text()

    header = header.replace("LzwEncoder", "LzwEncoderInst")
    header = header.replace("FLEXIGIF_LZWENCODERInst_HPP", "GIFOUT_LZWENC_INST_HPP")
    header = header.replace("FLEXIGIF_LZWENCODER_HPP", "GIFOUT_LZWENC_INST_HPP")
    source = source.replace("LzwEncoder", "LzwEncoderInst")
    source = source.replace('#include "LzwEncoderInst.hpp"', '#include "LzwEncoder_inst.hpp"')

    (HERE / "LzwEncoder_inst.hpp").write_text(patch(header, HEADER_PATCH, "LzwEncoder.hpp"))
    (HERE / "LzwEncoder_inst.cpp").write_text(patch(source, SOURCE_PATCH, "LzwEncoder.cpp"))
    print(f"wrote {HERE}/LzwEncoder_inst.[ch]pp")


main()
