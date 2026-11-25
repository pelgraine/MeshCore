"""Accept the LR1121 transceiver firmware type used by ExpressLRS 3.6."""
from pathlib import Path

Import("env")

source = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") /
          "RadioLib/src/modules/LR11x0/LR11x0.cpp")
original = "if((versionInfo.device == ver) || (versionInfo.device == RADIOLIB_LR11X0_DEVICE_BOOT)) {"
patched = ("if((versionInfo.device == ver) || "
           "(ver == RADIOLIB_LR11X0_DEVICE_LR1121 && versionInfo.device == 0xF3) || "
           "(versionInfo.device == RADIOLIB_LR11X0_DEVICE_BOOT)) {")
text = source.read_text()
if original in text:
    source.write_text(text.replace(original, patched, 1))
elif patched not in text:
    raise RuntimeError("Unexpected RadioLib LR1121 detection code; cannot apply ExpressLRS compatibility patch")
