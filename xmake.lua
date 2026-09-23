set_xmakever("2.8.2")

includes("E:/CommonLibVR")

set_project("SleepInBedSKSE")
set_version("1.3.5")

set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")

add_requires("rapidjson", "simpleini", "minhook")

target("SleepInBedSKSE")
    add_deps("commonlibsse-ng")
    add_packages("rapidjson", "simpleini", "minhook")
    add_defines("SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE")

    add_rules("commonlibsse-ng.plugin", {
        name = "SleepInBedSKSE",
        author = "Elzar125",
        description = "Lie down before sleeping; followers share beds. CN build.",
        email = ""
    })

    add_files("src/**.cpp")
    add_headerfiles("include/**.h")
    add_includedirs("include", "extern")
    set_pcxxheader("include/PCH.h")
