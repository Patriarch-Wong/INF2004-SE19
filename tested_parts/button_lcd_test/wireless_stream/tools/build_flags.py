Import("env")

# RTTI is a C++ setting; do not pass it to vendor C sources.
env.Append(CXXFLAGS=["-fno-rtti"])
