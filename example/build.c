#include "../programmable_build.h"

typedef struct StaticLib {
    bool       success;
    prb_String libFile;
} StaticLib;

typedef enum DownloadStatus {
    DownloadStatus_Downloaded,
    DownloadStatus_Skipped,
    DownloadStatus_Failed,
} DownloadStatus;

typedef struct DownloadResult {
    DownloadStatus status;
    prb_String     downloadDir;
    prb_String     includeDir;
    prb_String     includeFlag;
} DownloadResult;

DownloadResult
downloadRepo(prb_String rootDir, prb_String name, prb_String downloadUrl, prb_String includeDirRelToDownload) {
    prb_String     downloadDir = prb_pathJoin(rootDir, name);
    DownloadStatus downloadStatus = DownloadStatus_Failed;
    if (!prb_isDirectory(downloadDir) || prb_directoryIsEmpty(downloadDir)) {
        prb_String           cmd = prb_fmtAndPrintln("git clone --depth 1 %s %s", downloadUrl, downloadDir);
        prb_CompletionStatus status = prb_execCmdAndWait(cmd);
        if (status == prb_CompletionStatus_Success) {
            downloadStatus = DownloadStatus_Downloaded;
        }
    } else {
        prb_fmtAndPrintln("skip git clone %s", name);
        downloadStatus = DownloadStatus_Skipped;
    }
    prb_String     includeDir = prb_pathJoin(downloadDir, includeDirRelToDownload);
    DownloadResult result = {
        .status = downloadStatus,
        .downloadDir = downloadDir,
        .includeDir = includeDir,
        .includeFlag = prb_fmt("-I%s", includeDir),
    };
    return result;
}

StaticLib
compileStaticLib(
    prb_String     name,
    prb_String     rootDir,
    prb_String     compileOutDir,
    prb_String     compileCmdStart,
    DownloadResult download,
    char**         compileSourcesRelToDownload,
    int32_t        compileSourcesRelToDownloadCount,
    prb_String*    extraCompileFlags,
    int32_t        extraCompileFlagsCount
) {
    prb_String objDir = prb_pathJoin(compileOutDir, name);
    prb_createDirIfNotExists(objDir);

    prb_String cmdStart = prb_fmt(
        "%s %s %s",
        compileCmdStart,
        download.includeFlag,
        prb_stringsJoin(extraCompileFlags, extraCompileFlagsCount, " ")
    );

#if prb_PLATFORM_WINDOWS
    prb_String pdbPath = prb_pathJoin(compileOutDir, prb_fmt("%s.pdb", name));
    prb_String pdbOutputFlag = prb_fmt("/Fd%s", pdbPath);
    cmdStart = prb_fmt("%s %s", cmdStart, pdbOutputFlag);
#endif

    int32_t     compileSourcesCount = compileSourcesRelToDownloadCount;
    prb_String* compileSources = prb_allocArray(prb_String, compileSourcesCount);
    for (int32_t sourceIndex = 0; sourceIndex < compileSourcesCount; sourceIndex++) {
        compileSources[sourceIndex] = prb_pathJoin(download.downloadDir, compileSourcesRelToDownload[sourceIndex]);
    }

    int32_t          allInputMatchesCount = compileSourcesCount;
    prb_StringArray* allInputMatches = prb_allocArray(prb_StringArray, allInputMatchesCount);
    int32_t          allInputFilepathsCount = 0;
    for (int32_t inputPatternIndex = 0; inputPatternIndex < compileSourcesCount; inputPatternIndex++) {
        prb_String      inputPattern = compileSources[inputPatternIndex];
        prb_StringArray inputMatches = prb_getAllMatches(inputPattern);
        // TODO(khvorov) Report no matches
        allInputMatches[inputPatternIndex] = inputMatches;
        allInputFilepathsCount += inputMatches.len;
    }

    // NOTE(khvorov) Recompile everything whenever any .h file changes
    // TODO(khvorov) Probably just search the whole directory recursively for .h files
    prb_String hfilesInIncludePattern = prb_pathJoin(download.includeDir, "*.h");
    uint64_t   latestHFileChange = prb_getLatestLastModifiedFromPattern(hfilesInIncludePattern);
    for (int32_t inputMatchIndex = 0; inputMatchIndex < allInputMatchesCount; inputMatchIndex++) {
        prb_StringArray inputMatch = allInputMatches[inputMatchIndex];
        for (int32_t inputFilepathIndex = 0; inputFilepathIndex < inputMatch.len; inputFilepathIndex++) {
            prb_String inputFilepath = inputMatch.ptr[inputFilepathIndex];
            prb_String inputDir = prb_getParentDir(inputFilepath);
            prb_String adjacentHFilesPattern = prb_pathJoin(inputDir, "*.h");
            latestHFileChange = prb_max(latestHFileChange, prb_getLatestLastModifiedFromPattern(adjacentHFilesPattern));
        }
    }

    prb_String*        allOutputFilepaths = prb_allocArray(prb_String, allInputFilepathsCount);
    prb_ProcessHandle* processes = prb_allocArray(prb_ProcessHandle, allInputFilepathsCount);
    int32_t            processCount = 0;
    int32_t            allOutputFilepathsCount = 0;
    for (int32_t inputMatchIndex = 0; inputMatchIndex < allInputMatchesCount; inputMatchIndex++) {
        prb_StringArray inputMatch = allInputMatches[inputMatchIndex];
        for (int32_t inputFilepathIndex = 0; inputFilepathIndex < inputMatch.len; inputFilepathIndex++) {
            prb_String inputFilepath = inputMatch.ptr[inputFilepathIndex];
            prb_String inputFilename = prb_getLastEntryInPath(inputFilepath);
            prb_String outputFilename = prb_replaceExt(inputFilename, "obj");
            prb_String outputFilepath = prb_pathJoin(objDir, outputFilename);

            allOutputFilepaths[allOutputFilepathsCount++] = outputFilepath;

            uint64_t sourceLastMod = prb_getLatestLastModifiedFromPattern(inputFilepath);
            uint64_t outputLastMod = prb_getEarliestLastModifiedFromPattern(outputFilepath);

            if (sourceLastMod > outputLastMod || latestHFileChange > outputLastMod) {
#if prb_PLATFORM_WINDOWS
                prb_fmt("/Fo%s/", objDir);
#elif prb_PLATFORM_LINUX
                prb_String cmd = prb_fmt("%s -c -o %s %s", cmdStart, outputFilepath, inputFilepath);
#endif
                prb_println(cmd);
                processes[processCount++] = prb_execCmdAndDontWait(cmd);
            }
        }
    }

    if (processCount == 0) {
        prb_fmtAndPrintln("skip compile %s", name);
    }

    StaticLib            result = {0};
    prb_CompletionStatus compileStatus = prb_waitForProcesses(processes, processCount);
    if (compileStatus == prb_CompletionStatus_Success) {
#if prb_PLATFORM_WINDOWS
            prb_String staticLibFileExt = "lib");
#elif prb_PLATFORM_LINUX
        prb_String staticLibFileExt = "a";
#endif
            prb_String libFile = prb_pathJoin(compileOutDir, prb_fmt("%s.%s", name, staticLibFileExt));

            prb_String objsPathsString = prb_stringsJoin(allOutputFilepaths, allOutputFilepathsCount, " ");
#if prb_PLATFORM_WINDOWS
            prb_String libCmd = prb_fmt("lib /nologo -out:%s %s", libFile, objsPattern);
#elif prb_PLATFORM_LINUX
        prb_String libCmd = prb_fmt("ar rcs %s %s", libFile, objsPathsString);
#endif

            uint64_t sourceLastMod = prb_getLatestLastModifiedFromPatterns(allOutputFilepaths, allOutputFilepathsCount);
            uint64_t outputLastMod = prb_getEarliestLastModifiedFromPattern(libFile);
            prb_CompletionStatus libStatus = prb_CompletionStatus_Success;
            if (sourceLastMod > outputLastMod) {
                prb_println(libCmd);
                prb_removeFileIfExists(libFile);
                libStatus = prb_execCmdAndWait(libCmd);
            } else {
                prb_fmtAndPrintln("skip lib %s", name);
            }

            if (libStatus == prb_CompletionStatus_Success) {
                result = (StaticLib) {.success = true, .libFile = libFile};
            }
    }

    return result;
}

int
main() {
    // TODO(khvorov) Argument parsing
    // TODO(khvorov) Release build
    // TODO(khvorov) Clone a specific commit probably
    prb_init();
    prb_TimeStart scriptStartTime = prb_timeStart();

    prb_String rootDir = prb_getParentDir(__FILE__);

    prb_String compileOutDir = prb_pathJoin(rootDir, "build-debug");
    prb_createDirIfNotExists(compileOutDir);

#if prb_PLATFORM_WINDOWS
    prb_String compileCmdStart = "cl /nologo /diagnostics:column /FC /Zi";
#elif prb_PLATFORM_LINUX
    prb_String compileCmdStart = "gcc -g";
#endif

    //
    // SECTION ICU
    //

    prb_String     icuName = "icu";
    DownloadResult icuDownload =
        downloadRepo(rootDir, icuName, "https://github.com/unicode-org/icu", "icu4c/source/common");

    if (icuDownload.status == DownloadStatus_Failed) {
        return 1;
    }

    char* icuCompileSources[] = {
        "icu4c/source/common/uchar.cpp",
        "icu4c/source/common/utrie.cpp",
        "icu4c/source/common/utrie2.cpp",
        "icu4c/source/common/cmemory.cpp",
        "icu4c/source/common/utf_impl.cpp",
        "icu4c/source/common/normalizer2.cpp",
        "icu4c/source/common/normalizer2impl.cpp",
        "icu4c/source/common/uobject.cpp",
        "icu4c/source/common/edits.cpp",
        "icu4c/source/common/unistr.cpp",
        "icu4c/source/common/appendable.cpp",
        "icu4c/source/common/ustring.cpp",
        "icu4c/source/common/cstring.cpp",
        "icu4c/source/common/uinvchar.cpp",
        "icu4c/source/common/udataswp.cpp",
        "icu4c/source/common/putil.cpp",
        "icu4c/source/common/charstr.cpp",
        "icu4c/source/common/umutex.cpp",
        "icu4c/source/common/ucln_cmn.cpp",
        "icu4c/source/common/utrace.cpp",
        "icu4c/source/common/stringpiece.cpp",
        "icu4c/source/common/ustrtrns.cpp",
        "icu4c/source/common/util.cpp",
        "icu4c/source/common/patternprops.cpp",
        "icu4c/source/common/uniset.cpp",
        "icu4c/source/common/unifilt.cpp",
        "icu4c/source/common/unifunct.cpp",
        "icu4c/source/common/uvector.cpp",
        "icu4c/source/common/uarrsort.cpp",
        "icu4c/source/common/unisetspan.cpp",
        "icu4c/source/common/bmpset.cpp",
        "icu4c/source/common/ucptrie.cpp",
        "icu4c/source/common/bytesinkutil.cpp",
        "icu4c/source/common/bytestream.cpp",
        "icu4c/source/common/umutablecptrie.cpp",
        "icu4c/source/common/utrie_swap.cpp",
        "icu4c/source/common/ubidi_props.cpp",
        "icu4c/source/common/uprops.cpp",
        "icu4c/source/common/unistr_case.cpp",
        "icu4c/source/common/ustrcase.cpp",
        "icu4c/source/common/ucase.cpp",
        "icu4c/source/common/loadednormalizer2impl.cpp",
        "icu4c/source/common/uhash.cpp",
        "icu4c/source/common/udatamem.cpp",
        "icu4c/source/common/ucmndata.cpp",
        "icu4c/source/common/umapfile.cpp",
        "icu4c/source/common/udata.cpp",
        "icu4c/source/common/emojiprops.cpp",
        "icu4c/source/common/ucharstrieiterator.cpp",
        "icu4c/source/common/uvectr32.cpp",
        "icu4c/source/common/umath.cpp",
        "icu4c/source/common/ucharstrie.cpp",
        "icu4c/source/common/propname.cpp",
        "icu4c/source/common/bytestrie.cpp",
        "icu4c/source/stubdata/stubdata.cpp", // NOTE(khvorov) We won't need to access data here
    };

    char* icuFlags[] = {
        "-DU_COMMON_IMPLEMENTATION=1",
        "-DU_COMBINED_IMPLEMENTATION=1",
        "-DU_STATIC_IMPLEMENTATION=1",
    };

    // prb_clearDirectory(prb_pathJoin(compileOutDir, icuName));
    StaticLib icu = compileStaticLib(
        icuName,
        rootDir,
        compileOutDir,
        compileCmdStart,
        icuDownload,
        icuCompileSources,
        prb_arrayLength(icuCompileSources),
        icuFlags,
        prb_arrayLength(icuFlags)
    );

    if (!icu.success) {
        return 1;
    }

    //
    // SECTION Freetype and harfbuzz (they depend on each other)
    //

    char*          freetypeName = "freetype";
    DownloadResult freetypeDownload =
        downloadRepo(rootDir, freetypeName, "https://github.com/freetype/freetype", "include");
    if (freetypeDownload.status == DownloadStatus_Failed) {
        return 1;
    }

    prb_String     harfbuzzName = "harfbuzz";
    DownloadResult harfbuzzDownload =
        downloadRepo(rootDir, harfbuzzName, "https://github.com/harfbuzz/harfbuzz", "src");
    if (harfbuzzDownload.status == DownloadStatus_Failed) {
        return 1;
    }

    char* freetypeCompileSources[] = {
        // Required
        //"src/base/ftsystem.c", // NOTE(khvorov) Memory routines for freetype are in the main program
        "src/base/ftinit.c",
        "src/base/ftdebug.c",
        "src/base/ftbase.c",

        // Recommended
        "src/base/ftbbox.c",
        "src/base/ftglyph.c",

        // Optional
        "src/base/ftbdf.c",
        "src/base/ftbitmap.c",
        "src/base/ftcid.c",
        "src/base/ftfstype.c",
        "src/base/ftgasp.c",
        "src/base/ftgxval.c",
        "src/base/ftmm.c",
        "src/base/ftotval.c",
        "src/base/ftpatent.c",
        "src/base/ftpfr.c",
        "src/base/ftstroke.c",
        "src/base/ftsynth.c",
        "src/base/fttype1.c",
        "src/base/ftwinfnt.c",

        // Font drivers
        "src/bdf/bdf.c",
        "src/cff/cff.c",
        "src/cid/type1cid.c",
        "src/pcf/pcf.c",
        "src/pfr/pfr.c",
        "src/sfnt/sfnt.c",
        "src/truetype/truetype.c",
        "src/type1/type1.c",
        "src/type42/type42.c",
        "src/winfonts/winfnt.c",

        // Rasterisers
        "src/raster/raster.c",
        "src/sdf/sdf.c",
        "src/smooth/smooth.c",
        "src/svg/svg.c",

        // Auxillary
        "src/autofit/autofit.c",
        "src/cache/ftcache.c",
        "src/gzip/ftgzip.c",
        "src/lzw/ftlzw.c",
        "src/bzip2/ftbzip2.c",
        "src/gxvalid/gxvalid.c",
        "src/otvalid/otvalid.c",
        "src/psaux/psaux.c",
        "src/pshinter/pshinter.c",
        "src/psnames/psnames.c",
    };

    char* freetypeCompileFlags[] = {
        harfbuzzDownload.includeFlag,
        "-DFT2_BUILD_LIBRARY",
        "-DFT_CONFIG_OPTION_DISABLE_STREAM_SUPPORT",
        "-DFT_CONFIG_OPTION_USE_HARFBUZZ",
    };

    // prb_clearDirectory(prb_pathJoin(compileOutDir, freetypeName));
    StaticLib freetype = compileStaticLib(
        freetypeName,
        rootDir,
        compileOutDir,
        compileCmdStart,
        freetypeDownload,
        freetypeCompileSources,
        prb_arrayLength(freetypeCompileSources),
        freetypeCompileFlags,
        prb_arrayLength(freetypeCompileFlags)
    );

    if (!freetype.success) {
        return 1;
    };

    char* harfbuzzCompileSources[] = {
        "src/harfbuzz.cc",
        "src/hb-icu.cc",
    };

    char* harfbuzzCompileFlags[] = {
        icuDownload.includeFlag,
        freetypeDownload.includeFlag,
        "-DHAVE_ICU=1",
        "-DHAVE_FREETYPE=1",
        "-DHB_CUSTOM_MALLOC=1",
    };

    // prb_clearDirectory(prb_pathJoin(compileOutDir, harfbuzzName));
    StaticLib harfbuzz = compileStaticLib(
        harfbuzzName,
        rootDir,
        compileOutDir,
        compileCmdStart,
        harfbuzzDownload,
        harfbuzzCompileSources,
        prb_arrayLength(harfbuzzCompileSources),
        harfbuzzCompileFlags,
        prb_arrayLength(harfbuzzCompileFlags)
    );

    if (!harfbuzz.success) {
        return 1;
    };

    //
    // SECTION SDL
    //

    char* sdlCompileSources[] = {
        "src/atomic/*.c",
        "src/thread/*.c",
        "src/events/*.c",
        "src/file/*.c",
        "src/stdlib/*.c",
        "src/libm/*.c",
        "src/locale/*.c",
        "src/timer/*.c",
        "src/video/*.c",
        "src/video/dummy/*.c",
        "src/video/yuv2rgb/*.c",
        "src/render/*.c",
        "src/render/software/*.c",
        "src/cpuinfo/*.c",
        "src/*.c",
#if prb_PLATFORM_WINDOWS
        "src/core/windows/windows.c",
        "src/filesystem/windows/*.c",
        "src/timer/windows/*.c",
        "src/thread/windows/*.c",
        "src/video/windows/*.c",
        "src/loadso/windows/*.c",
        "src/locale/windows/*.c",
        "src/main/windows/*.c",
#elif prb_PLATFORM_LINUX
        "src/thread/pthread/*.c",
        "src/timer/unix/*.c",
        "src/filesystem/unix/*.c",
        "src/loadso/dlopen/*.c",
        "src/video/x11/*.c",
        "src/core/unix/SDL_poll.c",
        "src/core/linux/SDL_threadprio.c",
#endif
    };

    char* sdlCompileFlags[] = {
        "-DSDL_AUDIO_DISABLED=1",
        "-DSDL_HAPTIC_DISABLED=1",
        "-DSDL_HIDAPI_DISABLED=1",
        "-DSDL_SENSOR_DISABLED=1",
        "-DSDL_JOYSTICK_DISABLED=1",
        "-DSDL_VIDEO_RENDER_D3D=0",
        "-DSDL_VIDEO_RENDER_D3D11=0",
        "-DSDL_VIDEO_RENDER_D3D12=0",
        "-DSDL_VIDEO_RENDER_OGL=0",
        "-DSDL_VIDEO_RENDER_OGL_ES2=0",
#if prb_PLATFORM_LINUX
        "-Wno-deprecated-declarations",
        "-DHAVE_STRING_H=1",
        "-DHAVE_STDIO_H=1",
        "-DSDL_TIMER_UNIX=1",
        "-DSDL_FILESYSTEM_UNIX=1",
        "-DSDL_LOADSO_DLOPEN=1",
        "-DSDL_VIDEO_DRIVER_X11=1",
        "-DSDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS=1",
        "-DNO_SHARED_MEMORY=1",
        "-DSDL_THREAD_PTHREAD=1",
        "-DHAVE_NANOSLEEP=1",
        "-DHAVE_CLOCK_GETTIME=1",
        "-DCLOCK_MONOTONIC_RAW=1",
#endif
    };

    char*          sdlName = "sdl";
    DownloadResult sdlDownload = downloadRepo(rootDir, sdlName, "https://github.com/libsdl-org/SDL", "include");
    if (sdlDownload.status == DownloadStatus_Failed) {
        return 1;
    }

    if (sdlDownload.status == DownloadStatus_Downloaded) {
        prb_String downloadDir = sdlDownload.downloadDir;

        // NOTE(khvorov) Purge dynamic api because otherwise you have to compile a lot more of sdl
        prb_String dynapiPath = prb_pathJoin(downloadDir, "src/dynapi/SDL_dynapi.h");
        prb_textfileReplace(dynapiPath, "#define SDL_DYNAMIC_API 1", "#define SDL_DYNAMIC_API 0");

        // NOTE(khvorov) This XMissingExtension function is in X11 extensions and SDL doesn't use it.
        // Saves us from having to -lXext for no reason
        prb_String x11sym = prb_pathJoin(downloadDir, "src/video/x11/SDL_x11sym.h");
        prb_textfileReplace(
            x11sym,
            "SDL_X11_SYM(int,XMissingExtension,(Display* a,_Xconst char* b),(a,b),return)",
            "//SDL_X11_SYM(int,XMissingExtension,(Display* a,_Xconst char* b),(a,b),return"
        );

        // NOTE(khvorov) SDL would normally overwrite this "minimal" config with a generated one on linux
        prb_String configMinimal = prb_pathJoin(downloadDir, "include/SDL_config_minimal.h");
        prb_textfileReplace(configMinimal, "#define SDL_THREADS_DISABLED    1", "#define SDL_THREADS_DISABLED    0");
        prb_textfileReplace(configMinimal, "#define SDL_TIMERS_DISABLED 1", "#define SDL_TIMERS_DISABLED 0");

        // NOTE(khvorov) SDL allocates the pixels in the X11 framebuffer using
        // SDL_malloc but then frees it using XDestroyImage which will call libc
        // free. So even SDL's own custom malloc won't work because libc free will
        // crash when trying to free a pointer allocated with something other than
        // libc malloc.
        prb_String x11FrameBuffer = prb_pathJoin(downloadDir, "src/video/x11/SDL_x11framebuffer.c");
        prb_textfileReplace(
            x11FrameBuffer,
            "XDestroyImage(data->ximage);",
            "SDL_free(data->ximage->data);data->ximage->data = 0;XDestroyImage(data->ximage);"
        );
    }

    StaticLib sdl = compileStaticLib(
        sdlName,
        rootDir,
        compileOutDir,
        compileCmdStart,
        sdlDownload,
        sdlCompileSources,
        prb_arrayLength(sdlCompileSources),
        sdlCompileFlags,
        prb_arrayLength(sdlCompileFlags)
    );

    if (!sdl.success) {
        return 1;
    }

    //
    // SECTION Pack font into a C array
    //

    prb_String fontFilePath = prb_pathJoin(rootDir, "LiberationMono-Regular.ttf");
    prb_String fontArrayPath = prb_pathJoin(rootDir, "fontdata.c");
    if (!prb_isFile(fontArrayPath)) {
        prb_binaryToCArray(fontFilePath, fontArrayPath, "fontdata");
    }

    //
    // SECTION Main program
    //

    prb_String mainFlags[] = {
        freetypeDownload.includeFlag,
        sdlDownload.includeFlag,
        harfbuzzDownload.includeFlag,
        icuDownload.includeFlag,
        "-Wall -Wextra -Wno-unused-function",
#if prb_PLATFORM_WINDOWS
        "-Zi"),
        prb_stringJoin2("-Fo"), prb_pathJoin(compileOutDir, "example.obj"))),
        prb_stringJoin2("-Fe"), prb_pathJoin(compileOutDir, "example.exe"))),
        prb_stringJoin2("-Fd"), prb_pathJoin(compileOutDir, "example.pdb"))),
#elif prb_PLATFORM_LINUX
        prb_fmt("-o %s/example.bin", compileOutDir),
#endif
    };

    prb_String mainFiles[] = {
        prb_pathJoin(rootDir, "example.c"),
        freetype.libFile,
        sdl.libFile,
        harfbuzz.libFile,
        icu.libFile,
#if prb_PLATFORM_LINUX
#endif
    };

#if prb_PLATFORM_WINDOWS
    prb_String mainLinkFlags =
        " -link -incremental:no -subsystem:windows "
        "User32.lib ";
#elif prb_PLATFORM_LINUX
    // TODO(khvorov) Get rid of -lm and -ldl
    prb_String mainLinkFlags = "-lX11 -lpthread -lm -lstdc++ -ldl";
#endif

    prb_String mainCmd = prb_fmtAndPrintln(
        "%s %s %s %s",
        compileCmdStart,
        prb_stringsJoin(mainFlags, prb_arrayLength(mainFlags), " "),
        prb_stringsJoin(mainFiles, prb_arrayLength(mainFiles), " "),
        mainLinkFlags
    );

    prb_CompletionStatus mainStatus = prb_execCmdAndWait(mainCmd);

    if (mainStatus == prb_CompletionStatus_Success) {
        prb_fmtAndPrintln("total: %.2fms", prb_getMsFrom(scriptStartTime));
    } else {
        return 1;
    }

    return 0;
}
