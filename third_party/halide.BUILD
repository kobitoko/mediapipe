# Copyright 2023 The MediaPipe Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@halide//:halide.bzl", "halide_language_copts")

licenses(["notice"])

package(
    default_visibility = ["//visibility:public"],
)

cc_library(
    name = "language",
    hdrs = ["include/Halide.h"],
    copts = halide_language_copts(),
    includes = ["include"],
    deps = [
        ":runtime",
    ],
)

cc_library(
    name = "runtime",
    hdrs = glob([
        "include/HalideRuntime*.h",
        "include/HalideBuffer*.h",
    ]),
    includes = ["include"],
)

cc_library(
    name = "lib_halide_static",
    srcs = select({
        "@halide//:halide_config_windows_x86_64": [
            "lib/Release/Halide.lib",
            "bin/Release/Halide.dll",
        ],
        "//conditions:default": [
            "lib/libHalide.a",
        ],
    }),
    visibility = ["//visibility:private"],
)

cc_library(
    name = "gengen",
    srcs = [
        "share/Halide/tools/GenGen.cpp",
    ],
    includes = [
        "include",
        "share/Halide/tools",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":language",
        ":lib_halide_static",
    ],
)
