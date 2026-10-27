set_xmakever("3.0.0")

set_project("HardLevelCap")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")

includes("extern/CommonLibF4")

add_requires("minhook")

target("HardLevelCap", function()
    add_rules("commonlibf4.plugin", {
        name = "HardLevelCap",
        author = "HardLevelCap",
        description = "Stops leveling and XP popups at a chosen level cap"
    })

    add_packages("minhook")
    add_files("src/*.cpp")
end)
