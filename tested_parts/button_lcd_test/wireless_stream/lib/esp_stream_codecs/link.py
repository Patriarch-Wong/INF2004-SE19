from pathlib import Path

Import("env")
env.Append(LIBPATH=[str(Path(env.subst("$PROJECT_DIR")) /
                        "lib/esp_stream_codecs/prebuilt")])
env.Append(LIBS=["tinyh264", "esp_audio_codec"])
