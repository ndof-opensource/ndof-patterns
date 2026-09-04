from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class Package(ConanFile):
    name = "ndof-patterns"
    version = "0.1.0"
    description = "NDoF design patterns"
    license = "Apache-2.0"
    url = "https://github.com/ndof-opensource/ndof-patterns"
    topics = ("ndof",)

    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    generators = "CMakeToolchain", "CMakeDeps"
    exports_sources = (
        "CMakeLists.txt",
        "CMakePresets.json",
        "cmake/*",
        "include/*",
        "src/*",
        "tests/*",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def build_requirements(self):
        self.test_requires("gtest/1.15.0")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if not self.conf.get("tools.build:skip_test", default=False):
            cmake.test()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["ndof-patterns"]
        self.cpp_info.set_property("cmake_file_name", "ndof-patterns")
        self.cpp_info.set_property("cmake_target_name", "ndof::patterns")
