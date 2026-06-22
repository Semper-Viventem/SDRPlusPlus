cmake_minimum_required(VERSION 3.13)
project(nrsc5_decoder)

file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(nrsc5_decoder PRIVATE "src/")

# libnrsc5 (HD Radio / NRSC-5 decoder library): https://github.com/theori-io/nrsc5
# It does not install a pkg-config (.pc) file, so locate it manually. Override the
# search location with -DNRSC5_DIR=<prefix> or -DCMAKE_PREFIX_PATH=<prefix> if needed.
if (ANDROID)
    target_include_directories(nrsc5_decoder PUBLIC /sdr-kit/${ANDROID_ABI}/include)
    target_link_libraries(nrsc5_decoder PUBLIC /sdr-kit/${ANDROID_ABI}/lib/libnrsc5.so)
else ()
    find_path(NRSC5_INCLUDE_DIR
        NAMES nrsc5.h
        HINTS ENV NRSC5_DIR
        PATHS "$ENV{HOME}/.local" /usr/local /usr /opt/local
        PATH_SUFFIXES include)
    find_library(NRSC5_LIBRARY
        NAMES nrsc5
        HINTS ENV NRSC5_DIR
        PATHS "$ENV{HOME}/.local" /usr/local /usr /opt/local
        PATH_SUFFIXES lib lib64)

    if (NOT NRSC5_INCLUDE_DIR OR NOT NRSC5_LIBRARY)
        message(FATAL_ERROR "libnrsc5 not found. Build it from https://github.com/theori-io/nrsc5 "
                            "and install it (e.g. to ~/.local or /usr/local), then set "
                            "-DNRSC5_DIR=<prefix> or -DCMAKE_PREFIX_PATH=<prefix> if it lives in a custom location.")
    endif ()

    target_include_directories(nrsc5_decoder PRIVATE ${NRSC5_INCLUDE_DIR})
    target_link_libraries(nrsc5_decoder PRIVATE ${NRSC5_LIBRARY})

    # Ensure the dynamic loader can find libnrsc5.so at runtime when it lives in a
    # non-standard prefix (e.g. ~/.local/lib).
    get_filename_component(NRSC5_LIBRARY_DIR "${NRSC5_LIBRARY}" DIRECTORY)
    set_target_properties(nrsc5_decoder PROPERTIES
        BUILD_RPATH "${NRSC5_LIBRARY_DIR}"
        INSTALL_RPATH "${NRSC5_LIBRARY_DIR}")
endif ()
