set_project("photo-ops")
set_version("1.8.0")
set_xmakever("3.1.2")
set_languages("c++26")
add_cxxflags("-fcontracts", "-fcontract-evaluation-semantic=quick_enforce")
add_ldflags("-fcontracts")
add_rules("mode.debug", "mode.release")
set_warnings("all", "extra", "error")
set_policy("build.c++.modules", true)
set_policy("build.c++.modules.std", false)

option("exr")
    set_default(false)
    set_showmenu(true)
    set_description("Enable float32 OpenEXR output")
option_end()

option("sanitize")
    set_default(false)
    set_showmenu(true)
    set_description("Enable AddressSanitizer and UndefinedBehaviorSanitizer")
option_end()
if has_config("sanitize") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer")
    add_ldflags("-fsanitize=address,undefined")
end

-- pkg-config keeps dependency discovery independent of the package manager.
-- System packages are canonical; PHOTO_OPS_DEPS=brew explicitly opts into Homebrew.
add_requires("pkgconfig::libraw", {alias = "raw", system = true})
add_requires("pkgconfig::libtiff-4", {alias = "tiff", system = true})
add_requires("pkgconfig::libturbojpeg >=3.2.0", {alias = "jpeg", system = true})
add_requires("pkgconfig::lcms2", {alias = "lcms", system = true})
add_requires("pkgconfig::jsoncpp", {alias = "json", system = true})
add_requires("pkgconfig::opencv5", {alias = "opencv", system = true})
if has_config("exr") then
    add_requires("pkgconfig::OpenEXR", {alias = "exr", system = true})
end

target("photo_ops")
    set_kind("static")
    add_files("src/**.cppm", {public = true})
    add_files("src/io/*.cpp", "src/sunlight/*.cpp", "src/diagnostics/*.cpp",
              "src/focus_stack/*.cpp", "src/registration/*.cpp", "src/hdr/*.cpp")
    add_packages("raw", "tiff", "jpeg", "lcms", "opencv")
    if has_config("exr") then
        add_packages("exr")
        add_defines("PHOTO_OPS_EXR")
    end
    before_build(function ()
        os.vrunv("python", {"scripts/format.py", "--check"})
    end)
    on_config(function (target)
        -- The distro .pc exports every OpenCV component, including GUI/VTK backends.
        -- This adapter needs only features, similarity geometry and core arrays.
        target:pkg("opencv"):set("links", {"opencv_features", "opencv_geometry", "opencv_imgproc",
                                           "opencv_flann", "opencv_core"})
        local compiler = target:tool("cxx")
        local version = os.iorunv(compiler, {"-dumpfullversion"}):trim()
        assert(version:startswith("16.2."),
               "photo-ops requires GCC 16.2.x; configured compiler reports " .. version)
    end)

target("photo-ops")
    set_kind("binary")
    add_files("src/cli/main.cpp")
    add_packages("json")
    add_deps("photo_ops")

target("photo-ops-tests")
    set_kind("binary")
    add_files("tests/*.cpp")
    add_deps("photo_ops")
    add_packages("tiff", "lcms", "jpeg")
    if has_config("exr") then
        add_packages("exr")
        add_defines("PHOTO_OPS_EXR")
    end
    add_tests("foundation")

-- Formatting is explicit when mutating files and mandatory (read-only) during builds.
task("format")
    on_run(function ()
        os.execv("python", {"scripts/format.py"})
    end)
    set_menu {usage = "xmake format", description = "Apply the project C++ style"}

task("format-check")
    on_run(function ()
        os.execv("python", {"scripts/format.py", "--check"})
    end)
    set_menu {usage = "xmake format-check", description = "Check C++ formatting without edits"}
